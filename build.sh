#!/bin/sh
# web-only build: compiles the engine into web/earth.js + web/earth.wasm as a
# modularized library (createEarthModule) for web/index.html. the native and
# standalone-emscripten targets were removed — see the root README's
# "stuff we've removed" section to dig them out of git history.
source config_emscripten.sh
echo build: weblib
$EMSCRIPTEN_PROTOBUF_EXE --cpp_out=. proto/rocktree.proto
cd crn && emcc -std=c++14 -O2 -c crn.cc -w && cd ..

mkdir -p web
cp coi-serviceworker.js web/
emcc earth_web.cpp -O2 -std=c++17 -Wno-deprecated-declarations -I. -I./eigen/ \
	-I$EMSCRIPTEN_PROTOBUF_SRC $EMSCRIPTEN_PROTOBUF_LIB crn/crn.o \
	-DEARTH_WEBLIB \
	-s MALLOC=mimalloc -Wno-pthreads-mem-growth \
	-s USE_SDL=2 -s FETCH=1 -s USE_PTHREADS=1 \
	-s INITIAL_MEMORY=536870912 -s ALLOW_MEMORY_GROWTH=1 -s MAXIMUM_MEMORY=4294967296 \
	-s PTHREAD_POOL_SIZE="'navigator.hardwareConcurrency'" \
	--bind -s MODULARIZE=1 -s EXPORT_NAME=createEarthModule \
	-o web/earth.js
