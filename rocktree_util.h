
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

// ms timestamp for the timing: log lines (sdl-free so the web library can
// compile this header without sdl)
static inline uint32_t timing_ms() {
	using namespace std::chrono;
	return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

BulkMetadataRequest createBulkMetadataRequest(const std::string base_path, std::string path, int epoch);
NodeDataRequest createNodeDataRequest(const std::string base_path, BulkMetadata bulk, NodeMetadata node_meta);

// extended data structure using decoders
#include "rocktree_ex.h"

// createBulkMetadataRequest creates a object for requesting bulk data. can be re-used
BulkMetadataRequest createBulkMetadataRequest(const std::string base_path, std::string path, int epoch)
{
	BulkMetadataRequest req;
	auto key = new NodeKey();
	key->set_allocated_path(new std::string(base_path + path));
	key->set_epoch(epoch);
	req.set_allocated_node_key(key);
	return req;
}

// createNodeDataRequest creates a object for requesting node data. can be re-used
NodeDataRequest createNodeDataRequest(const std::string base_path, BulkMetadata bulk, NodeMetadata node_meta) {
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

	static std::map<int, std::function<void(std::unique_ptr<PlanetoidMetadata>)>> map;
	static int i = 0;

	auto thunk = [](int i, int error, uint8_t *data, size_t len){
		auto cb = map[i];		
  		if (error) {
			fprintf(stderr, "%s", "could not load planetoid\n");
			cb(NULL);
		} else {
			PlanetoidMetadata planetoid;			
			cb(planetoid.ParseFromArray(data, len) ? std::make_unique<PlanetoidMetadata>(planetoid) : NULL);
		}
		map.erase(i);
	};

	map[++i] = cb;
	fetchData("PlanetoidMetadata", i, thunk);
}

#include "threads.h"
// decode pool: protobuf parse + mesh/texture decompression. populateNode /
// populateBulk only write their own node/bulk and publish it with the atomic
// dl_state store, so decodes of distinct nodes can run concurrently. decode
// dominates warm-cache load time, so use most of the machine
ThreadPool pool(std::max(2, (int)std::thread::hardware_concurrency() - 2));
#ifndef EMSCRIPTEN
ThreadPool webpool(4);
#endif

// getBulk fetches a bulk using path and epoch from web or cache and calls cb when it's done.
void getBulk(BulkMetadataRequest req, rocktree_t::bulk_t *b, std::function<void(std::unique_ptr<BulkMetadata>)> cb) {		

	auto path = req.node_key().path().c_str();
	auto epoch = req.node_key().epoch();

	static std::mutex m;
	using p = std::pair<std::function<void(std::unique_ptr<BulkMetadata>)>, rocktree_t::bulk_t *>;
	static std::map<int, p> map;
	static int i = 0;

	auto thunk = [](int i, int error, uint8_t *data, size_t len){
		p pair;	
		{	
#ifndef EMSCRIPTEN
			std::lock_guard<std::mutex> lockGuard(m);
#endif
			auto it = map.find(i);
			pair = it->second;
			map.erase(it);
		}
		auto cb = pair.first;
		auto b = pair.second;
  		if (error) {
			// back to a stub, or this node is never asked for again: the walk
			// only ever schedules stubs, so a transport failure that left the
			// state at 'downloading' meant the tile was lost for the life of
			// the page, and the view could never complete. the scheduler backs
			// off between attempts (dl_next_try_ms) so a url that keeps failing
			// costs one request every few seconds, not one every frame
			b->setFailedDownloading();
			fprintf(stderr, "could not load bulk %s (attempt %d)\n",
				b->request.node_key().path().c_str(), b->dl_fails);
			cb(NULL);
		} else {
			auto vec = std::vector<uint8_t>(data, data+len);
			
			auto result = pool.enqueue([](auto b, auto cb, auto vec) {

				auto t0 = timing_ms();
				BulkMetadata bulk;

				if (!bulk.ParseFromArray(vec.data(), vec.size())) {
					printf("bulk parse failed: %s (%zu bytes)\n", b->request.node_key().path().c_str(), vec.size());
					b->setFailedDownloading();
					cb(NULL);
					return;
				}

				auto bu = std::make_unique<BulkMetadata>(bulk);
				populateBulk(b, std::move(bu));
				if (g_log_timing)
					printf("timing: decode bulk %s start=%u dur=%u\n",
						b->request.node_key().path().c_str(), t0, timing_ms() - t0);
				cb(NULL);

			}, b, cb, vec);
		}

	};

	assert(strlen(path) < 30);
	char *url_buf = new char[200];
	sprintf(url_buf, "BulkMetadata/pb=!1m2!1s%s!2u%d", path, epoch);	

#ifndef EMSCRIPTEN
	auto result = webpool.enqueue([](auto url_buf, auto i, auto thunk, auto cb, auto b) {
		{	
			std::lock_guard<std::mutex> lockGuard(m);
			map[i] = std::make_pair(cb, b);
		}
		fetchData(url_buf, i, thunk);
		delete [] url_buf;
	}, url_buf, ++i, thunk, cb, b);
#else
	++i;
	map[i] = std::make_pair(cb, b);
	fetchData(url_buf, i, thunk);
#endif
}

// getNode fetches a node from path, epoch, texture_format and imagery_epoch (none if -1) and calls cb when it's done.
void getNode(NodeDataRequest req, rocktree_t::node_t *n, std::function<void(std::unique_ptr<NodeData>)> cb) {

	static std::mutex m;
	using p = std::pair<std::function<void(std::unique_ptr<NodeData>)>, rocktree_t::node_t *>;
	static std::map<int, p> map;
	static int i = 0;	

	auto thunk = [](int i, int error, uint8_t *data, size_t len){
		p pair;	
		{	
#ifndef EMSCRIPTEN
			std::lock_guard<std::mutex> lockGuard(m);
#endif
			auto it = map.find(i);
			pair = it->second;
			map.erase(it);
		}	
		auto cb = pair.first;
		auto n = pair.second;
  		if (error) {
			// back to a stub, or this node is never asked for again: the walk
			// only ever schedules stubs, so a transport failure that left the
			// state at 'downloading' meant the tile was lost for the life of
			// the page, and the view could never complete. the scheduler backs
			// off between attempts (dl_next_try_ms) so a url that keeps failing
			// costs one request every few seconds, not one every frame
			n->setFailedDownloading();
			fprintf(stderr, "could not load node %s (attempt %d)\n",
				n->request.node_key().path().c_str(), n->dl_fails);
			cb(NULL);
		} else {			
			auto vec = std::vector<uint8_t>(data, data+len);

			auto result = pool.enqueue([](auto n, auto cb, auto vec) {
				auto t0 = timing_ms();
				NodeData node;

				if (!node.ParseFromArray(vec.data(), vec.size())) {
					printf("node parse failed: %s (%zu bytes)\n", n->request.node_key().path().c_str(), vec.size());
					n->setFailedDownloading();
					cb(NULL);
					return;
				}

				auto nu = std::make_unique<NodeData>(node);
				populateNode(n, std::move(nu));
				if (g_log_timing)
					printf("timing: decode node %s start=%u dur=%u\n",
						n->request.node_key().path().c_str(), t0, timing_ms() - t0);
				cb(NULL);
    		}, n, cb, vec);		
		}
	};

	auto path = req.node_key().path().c_str();

	assert(strlen(path) < 30);
	char *url_buf = new char[200];
	if (!req.has_imagery_epoch()) {		
		sprintf(url_buf, "NodeData/pb=!1m2!1s%s!2u%d!2e%d!4b0", path, req.node_key().epoch(), req.texture_format());
	} else {
		sprintf(url_buf, "NodeData/pb=!1m2!1s%s!2u%d!2e%d!3u%d!4b0", path, req.node_key().epoch(), req.texture_format(), req.imagery_epoch());
	}	
	
#ifndef EMSCRIPTEN
	//fetchData(url_buf, i, thunk);
	auto result = webpool.enqueue([](auto url_buf, auto i, auto thunk, auto cb, auto n) {
		{	
			std::lock_guard<std::mutex> lockGuard(m);
			map[i] = std::make_pair(cb, n);
		}
		fetchData(url_buf, i, thunk);
		delete [] url_buf;
	}, url_buf, ++i, thunk, cb, n);
#else
	++i;
	map[i] = std::make_pair(cb, n);
	fetchData(url_buf, i, thunk);
#endif
}

struct llbounds_t {
	double n, s, w, e;
};

// latLonToOctant converts lat-lon to octant. incorrectly?
void latLonToOctant(double lat, double lon, char octant[MAX_LEVEL + 1]) {
	octant[0] = 0;
	octant[1] = 0;
	llbounds_t box;

	if (lat < 0.0) { octant[1] |= 2; box.n = 0.0; box.s = -90.0;}
	else { octant[0] |= 2; box.n = 90.0; box.s = 0.0; }

	if (lon < -90.0) { box.w = -180.0; box.e = -90.0; }
	else if (lon < 0.0) { octant[1] |= 1; box.w = -90.0; box.e = 0.0; }
	else if (lon < 90.0) { octant[0] |= 1; box.w = 0.0; box.e = 90.0; }
	else { octant[0] |= 1; octant[1] |= 1; box.w = 90.0; box.e = 180.0; }

	int level = MAX_LEVEL;
	for (int i = 2; i < level; i++) {
		octant[i] = 0;

		double mid_lat = (box.n + box.s) / 2.0;
		double mid_lon = (box.w + box.e) / 2.0;

		if (lat < mid_lat) {
			box.n = mid_lat;
		} else {
			box.s = mid_lat;
			octant[i] |= 2;
		}

		if (lon < mid_lon) {
			box.e = mid_lon;
		} else {
			box.w = mid_lon;
			octant[i] |= 1;
		}
	}

	// to ascii
	for (int i = 0; i < level; i++) octant[i] += '0';
	octant[MAX_LEVEL] = 0;
}