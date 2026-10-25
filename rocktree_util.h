
// protobuf
#include "proto/rocktree.pb.h"
#include "proto/rocktree.pb.cc"
using namespace geo_globetrotter_proto_rocktree;

// further decoding for protobuf
#include "rocktree_decoder.h"

// types after decoding for rendering
#include "rocktree_types.h"

// http(s) and caching
#include "rocktree_http.h"

// additional math
#include "rocktree_math.h"

// opengl helpers
#include "rocktree_gl.h"

// nice
#include <thread>
#include <algorithm>
#include <chrono>
#include <atomic>


// per-tile decode timing, off by default: it is one line per node and a
// settling view decodes hundreds, which buries everything else in the
// console. the shell turns it on (?timing on the web page) when the decode
// pool is what's actually being looked at. written once at startup and read
// from the decode threads, hence atomic
static std::atomic<bool> g_log_timing{false};

// ms timestamp for the timing: log lines
static inline uint32_t timing_ms() {
	using namespace std::chrono;
	return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

BulkMetadataRequest createBulkMetadataRequest(const std::string &base_path, const std::string &path, int epoch);
NodeDataRequest createNodeDataRequest(const std::string &base_path, const BulkMetadata &bulk, const NodeMetadata &node_meta);

// extended data structure using decoders
#include "rocktree_ex.h"

// createBulkMetadataRequest creates a object for requesting bulk data. can be re-used
BulkMetadataRequest createBulkMetadataRequest(const std::string &base_path, const std::string &path, int epoch)
{
	BulkMetadataRequest req;
	auto key = new NodeKey();
	key->set_allocated_path(new std::string(base_path + path));
	key->set_epoch(epoch);
	req.set_allocated_node_key(key);
	return req;
}

// createNodeDataRequest creates a object for requesting node data. can be re-used
NodeDataRequest createNodeDataRequest(const std::string &base_path, const BulkMetadata &bulk, const NodeMetadata &node_meta) {
	auto aux = unpackPathAndFlags(node_meta);	
	assert(!(aux.flags & NodeMetadata_Flags_NODATA));
	//assert(node_meta.has_epoch());
	NodeDataRequest req;

	// set texture format based on supported formats. crn/dxt1 preferred
	// (smaller, gpu-native) unless the render backend reported no s3tc
	req.set_texture_format(([&]() {
		static Texture_Format with_s3tc[] = { Texture_Format_CRN_DXT1, Texture_Format_JPG };
		static Texture_Format without_s3tc[] = { Texture_Format_JPG, Texture_Format_CRN_DXT1 };
		auto supported = texture_s3tc_supported ? with_s3tc : without_s3tc;

		int available = node_meta.has_available_texture_formats()
			? node_meta.available_texture_formats()
			: bulk.default_available_texture_formats();

		for (auto i = 0; i < 2; i++) {
			if (available & (1 << (supported[i] - 1))) return supported[i];
		}
		return supported[0];
	})());
	
	// set imagery epoch if flags say it should be used
	if (aux.flags & NodeMetadata_Flags_USE_IMAGERY_EPOCH) {
		auto imagery_epoch = node_meta.has_imagery_epoch() ?
			node_meta.imagery_epoch() :
			bulk.default_imagery_epoch();
		req.set_imagery_epoch(imagery_epoch);
	}

	// set path and epoch
	req.set_allocated_node_key(([&](){
		auto key = new NodeKey();
		key->set_allocated_path(new std::string(base_path + std::string(aux.path)));	
		assert(bulk.has_head_node_key() && bulk.head_node_key().has_epoch());
		key->set_epoch(node_meta.has_epoch() ? node_meta.epoch() : bulk.head_node_key().epoch());
		return key;
	})());

	return req;
}

// getPlanetoid fetches planetoid from web and calls cb when it's done.
void getPlanetoid(std::function<void(std::unique_ptr<PlanetoidMetadata>)> cb) {
	fetchData("PlanetoidMetadata", [cb](bool ok, const uint8_t *data, size_t len) {
  		if (!ok) {
			fprintf(stderr, "%s", "could not load planetoid\n");
			cb(NULL);
			return;
		}
		auto planetoid = std::make_unique<PlanetoidMetadata>();
		if (!planetoid->ParseFromArray(data, len)) planetoid = nullptr;
		cb(std::move(planetoid));
	});
}

#include "threads.h"
// decode pool: protobuf parse + mesh/texture decompression. populateNode /
// populateBulk only write their own node/bulk and publish it with the atomic
// dl_state store, so decodes of distinct nodes can run concurrently. decode
// dominates warm-cache load time, so use most of the machine
ThreadPool pool(std::max(2, (int)std::thread::hardware_concurrency() - 2));

// fetchAndDecode fetches url for x (a node or bulk), then parses it as a Msg
// and populates x on the decode pool. any failure — transport, parse, or the
// tile's data — puts x back to a stub. done runs at the end either way, on
// whichever thread got there. kind names x in the log lines
template <class Msg, class T>
void fetchAndDecode(const std::string &url, const char *kind, T *x,
	const char *(*populate)(T *, const Msg &), std::atomic<int> *phase, std::function<void()> done) {

	auto finish = [done]() { if (done) done(); };
	fetchData(url, [=](bool ok, const uint8_t *data, size_t len) {
  		if (!ok) {
			// back to a stub, or this node is never asked for again: the walk
			// only ever schedules stubs, so a transport failure that left the
			// state at 'downloading' meant the tile was lost for the life of
			// the page, and the view could never complete. the scheduler backs
			// off between attempts (dl_next_try_ms) so a url that keeps failing
			// costs one request every few seconds, not one every frame
			if (phase) *phase = fetch_phase_none;
			x->setFailedDownloading();
			fprintf(stderr, "could not load %s %s (attempt %d)\n",
				kind, x->request.node_key().path().c_str(), x->dl_fails);
			finish();
			return;
		}
		auto vec = std::vector<uint8_t>(data, data+len);
		if (phase) {
			*phase = fetch_phase_decode;
			if (g_log_timing)
				printf("timing: queued %s %s at=%u\n", kind, x->request.node_key().path().c_str(), timing_ms());
		}

		pool.post([=, vec = std::move(vec)]() {
			auto t0 = timing_ms();
			Msg msg;

			if (!msg.ParseFromArray(vec.data(), vec.size())) {
				printf("%s parse failed: %s (%zu bytes)\n", kind, x->request.node_key().path().c_str(), vec.size());
				x->setFailedDownloading();
				finish();
				return;
			}

			if (auto error = populate(x, msg)) {
				printf("%s decode failed: %s: %s (%zu bytes)\n", kind, x->request.node_key().path().c_str(), error, vec.size());
				x->setFailedDownloading();
				finish();
				return;
			}
			if (g_log_timing)
				printf("timing: decode %s %s start=%u dur=%u\n",
					kind, x->request.node_key().path().c_str(), t0, timing_ms() - t0);
			finish();
		});
	}, phase);
}

// getBulk fetches bulk b (b->request: path and epoch) from web or cache, and
// calls done when it's populated or has failed back to a stub.
void getBulk(rocktree_t::bulk_t *b, std::function<void()> done = nullptr) {
	const auto &key = b->request.node_key();
	auto url = "BulkMetadata/pb=!1m2!1s" + key.path() + "!2u" + std::to_string(key.epoch());
	fetchAndDecode<BulkMetadata>(url, "bulk", b, populateBulk, nullptr, done);
}

// getNode fetches node n (n->request: path, epoch, texture_format and
// imagery_epoch, if any) and calls done when it's populated or has failed back
// to a stub.
void getNode(rocktree_t::node_t *n, std::function<void()> done) {
	const auto &req = n->request;
	auto url = "NodeData/pb=!1m2!1s" + req.node_key().path()
		+ "!2u" + std::to_string(req.node_key().epoch())
		+ "!2e" + std::to_string(req.texture_format())
		+ (req.has_imagery_epoch() ? "!3u" + std::to_string(req.imagery_epoch()) : "")
		+ "!4b0";
	n->fetch_phase = fetch_phase_none;
	fetchAndDecode<NodeData>(url, "node", n, populateNode, &n->fetch_phase, done);
}
