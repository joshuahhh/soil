// fetch backend interface: resolve a resource path ("BulkMetadata/pb=...",
// "NodeData/pb=...", "PlanetoidMetadata") to bytes, calling thunk when done
// (from any thread). the built-in backends below are the default; shells can
// swap in their own (e.g. the web library delegates to a js function so the
// host app controls caching and transport)
typedef void (*fetch_thunk_t)(int i, int error, uint8_t *d, size_t l);
struct fetcher_t {
	virtual void fetch(const char *path, int i, fetch_thunk_t thunk) = 0;
	virtual ~fetcher_t() {}
};
fetcher_t *earth_fetcher = nullptr; // set below; shells may override

#ifdef EMSCRIPTEN
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
	void fetch(const char *path, int i, fetch_thunk_t thunk) override {
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
#else

#define HTTP_IMPLEMENTATION
#include "http.h"

std::once_flag cache_init_once_flag;

static const auto cache_pfx = "cache/";
static const auto cache_pfx_len = strlen(cache_pfx); // without 0
void createDir(const char* path);
bool readFile(const char* file_path, unsigned char** data, size_t* len);
void writeFile(const char* file_path, unsigned char* data, size_t len);

// disk cache in ./cache backed by plain http; runs synchronously on the
// calling (webpool) thread
struct http_cache_fetcher_t : fetcher_t {
	void fetch(const char *path, int i, fetch_thunk_t thunk) override;
};
static http_cache_fetcher_t default_fetcher;

void http_cache_fetcher_t::fetch(const char* path, int i, fetch_thunk_t thunk) {

	std::call_once(cache_init_once_flag, [](){
		createDir(cache_pfx);
		createDir((std::string(cache_pfx) + "BulkMetadata").c_str());
		createDir((std::string(cache_pfx) + "NodeData").c_str());
	});

	auto use_cache = path[0] != 'P'; // don't cache planetoid

	char *cache_path = (char*)malloc(cache_pfx_len + strlen(path) + 1);
	sprintf(cache_path, "%s%s", cache_pfx, path);
	{
		unsigned char* data; size_t len;
		if (use_cache && readFile(cache_path, &data, &len)) {
			printf("timing: cache %s at=%u\n", path, SDL_GetTicks());
			thunk(i, 0, data, len);
			free(data);
			free(cache_path);
			return;
		}
	}

	const char* base_url = "http://kh.google.com/rt/earth/";
	char* url = (char*)malloc(strlen(base_url) + strlen(path) + 1);
	strcpy(url, base_url); strcat(url, path);

	printf("timing: web %s at=%u\n", path, SDL_GetTicks());
	http_t* request = http_get(url, NULL);
	free(url);
	if (!request) {
		thunk(i, 1, NULL, 0);
		free(cache_path);
		return;
	}
	
	http_status_t status;
	do {
		status = http_process(request);
		SDL_Delay(1);
	} while (status == HTTP_STATUS_PENDING);

	// http.h reports e.g. 404/500 as COMPLETED, so check the status code
	// ourselves — an error body must not be treated (or cached) as data
	if (status == HTTP_STATUS_FAILED || request->status_code != 200) {
		fprintf(stderr, "http error %d: %s\n", request->status_code, path);
		http_release(request);
		thunk(i, 1, NULL, 0);
		free(cache_path);
		return;
	}

	auto data = (unsigned char*)malloc(request->response_size);
	auto len = request->response_size;
	memcpy(data, request->response_data, len);
	
	http_release(request);
	thunk(i, 0, data, len);

	if (use_cache) {
		writeFile(cache_path, data, len);
	}
	free(data);
	free(cache_path);
}

void createDir(const char* path) {
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, (mode_t)0755);
#endif
}

bool readFile(const char* file_path, unsigned char** data, size_t* len) {
	FILE* file = fopen(file_path, "rb");
	if (!file) return false;
	fseek(file, 0, SEEK_END);
	*len = ftell(file);
	if (*len == 0) { // treat empty files as a miss so they get refetched
		fclose(file);
		return false;
	}
	*data = (unsigned char*)malloc(*len);
	fseek(file, 0, SEEK_SET);
	auto ok = fread(*data, *len, 1, file) == 1;
	fclose(file);
	if (!ok) free(*data);
	return ok;
}

void writeFile(const char* file_path, unsigned char* data, size_t len) {
	// write to a temp file and rename, so an interrupted write can't leave a
	// truncated file that would be served as a valid cache entry forever
	auto tmp_path = std::string(file_path) + ".tmp";
	FILE* file = fopen(tmp_path.c_str(), "wb");
	if (!file) return;
	auto ok = len == 0 || fwrite(data, len, 1, file) == 1;
	fclose(file);
	if (!ok) {
		remove(tmp_path.c_str());
		return;
	}
#ifdef _WIN32
	remove(file_path); // windows rename() won't overwrite
#endif
	rename(tmp_path.c_str(), file_path);
}
#endif

// route requests through the active backend
void fetchData(const char* path, int i, fetch_thunk_t thunk) {
	if (!earth_fetcher) earth_fetcher = &default_fetcher;
	earth_fetcher->fetch(path, i, thunk);
}