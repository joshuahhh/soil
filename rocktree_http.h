#include <functional>
#include <string>

// fetch backend interface: resolve a resource path ("BulkMetadata/pb=...",
// "NodeData/pb=...", "PlanetoidMetadata") to bytes, calling done when it has
// them or has given up (from any thread). the bytes are only valid during the
// call. the built-in backend below is the default; shells can swap in their
// own (e.g. the web library delegates to a js function so the host app
// controls caching and transport)
typedef std::function<void(bool ok, const uint8_t *data, size_t len)> fetch_done_t;
// phase: where the request is, for the lod overlay (rocktree_types.h
// fetch_phase_*). a backend that can tell writes it as it goes; null when
// nobody is asking (bulks, the planetoid)
struct fetcher_t {
	virtual void fetch(const std::string &path, fetch_done_t done, std::atomic<int> *phase) = 0;
	virtual ~fetcher_t() {}
};
fetcher_t *earth_fetcher = nullptr; // set below; shells may override

#include <emscripten/fetch.h>

// userData is the request's fetch_done_t, owned by the request
// if you want to retain the data you need to copy it
void downloadSucceeded(emscripten_fetch_t *fetch) {
  auto done = (fetch_done_t*)(fetch->userData);
  (*done)(true, (const uint8_t*)fetch->data, (size_t)fetch->numBytes);
  delete done;
  emscripten_fetch_close(fetch);
}

void downloadFailed(emscripten_fetch_t *fetch) {
  //fprintf(stderr, "Downloading %s failed, HTTP failure status code: %d.\n", fetch->url, fetch->status);  
  auto done = (fetch_done_t*)(fetch->userData);
  (*done)(false, NULL, 0);
  delete done;
  emscripten_fetch_close(fetch);
}

// direct browser fetch of kh.google.com, no cache of our own (the browser's
// http cache applies)
struct emscripten_fetcher_t : fetcher_t {
	void fetch(const std::string &path, fetch_done_t done, std::atomic<int> *phase) override {
		if (phase) *phase = fetch_phase_net;
		// (emscripten_fetch keeps a copy of the url)
		auto url = "https://kh.google.com/rt/earth/" + path;

		emscripten_fetch_attr_t attr;
		emscripten_fetch_attr_init(&attr);

		attr.userData = (void*)new fetch_done_t(std::move(done));

		strcpy(attr.requestMethod, "GET");
		attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
		attr.onsuccess = downloadSucceeded;
		attr.onerror = downloadFailed;
		emscripten_fetch(&attr, url.c_str());
	}
};
static emscripten_fetcher_t default_fetcher;

// route requests through the active backend
void fetchData(const std::string &path, fetch_done_t done, std::atomic<int> *phase = nullptr) {
	if (!earth_fetcher) earth_fetcher = &default_fetcher;
	earth_fetcher->fetch(path, std::move(done), phase);
}
