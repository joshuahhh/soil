// fetch backend interface: resolve a resource path ("BulkMetadata/pb=...",
// "NodeData/pb=...", "PlanetoidMetadata") to bytes, calling thunk when done
// (from any thread). the built-in backend below is the default; shells can
// swap in their own (e.g. the web library delegates to a js function so the
// host app controls caching and transport)
typedef void (*fetch_thunk_t)(int i, int error, uint8_t *d, size_t l);
// phase: where the request is, for the lod overlay (rocktree_types.h
// fetch_phase_*). a backend that can tell writes it as it goes; null when
// nobody is asking (bulks, the planetoid)
struct fetcher_t {
	virtual void fetch(const char *path, int i, fetch_thunk_t thunk, std::atomic<int> *phase) = 0;
	virtual ~fetcher_t() {}
};
fetcher_t *earth_fetcher = nullptr; // set below; shells may override

#include <emscripten/fetch.h>

struct x {
	int i;
	void (*thunk)(int i, int error, uint8_t *d, size_t l);
};

// if you want to retain the data you need to copy it
void downloadSucceeded(emscripten_fetch_t *fetch) {
  auto xx = (x*)(fetch->userData);
  xx->thunk(xx->i, 0, (uint8_t*)fetch->data, (size_t)fetch->numBytes);
  delete xx;
  emscripten_fetch_close(fetch);
}

void downloadFailed(emscripten_fetch_t *fetch) {
  //fprintf(stderr, "Downloading %s failed, HTTP failure status code: %d.\n", fetch->url, fetch->status);  
  auto xx = (x*)(fetch->userData);
  xx->thunk(xx->i, 1, NULL, 0);
  delete xx;
  emscripten_fetch_close(fetch);
}

// direct browser fetch of kh.google.com, no cache of our own (the browser's
// http cache applies)
struct emscripten_fetcher_t : fetcher_t {
	void fetch(const char *path, int i, fetch_thunk_t thunk, std::atomic<int> *phase) override {
		if (phase) *phase = fetch_phase_net;
		const char* base_url = "https://kh.google.com/rt/earth/";
		char* url = (char*)malloc(strlen(base_url) + strlen(path) + 1);
		strcpy(url, base_url); strcat(url, path);

		emscripten_fetch_attr_t attr;
		emscripten_fetch_attr_init(&attr);

		auto xx = new x();
		xx->i = i;
		xx->thunk = thunk;
		attr.userData = (void*)xx;

		strcpy(attr.requestMethod, "GET");
		attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
		attr.onsuccess = downloadSucceeded;
		attr.onerror = downloadFailed;
		emscripten_fetch(&attr, url);
	}
};
static emscripten_fetcher_t default_fetcher;

// route requests through the active backend
void fetchData(const char* path, int i, fetch_thunk_t thunk, std::atomic<int> *phase = nullptr) {
	if (!earth_fetcher) earth_fetcher = &default_fetcher;
	earth_fetcher->fetch(path, i, thunk, phase);
}