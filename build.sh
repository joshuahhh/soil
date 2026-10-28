#!/bin/bash
# web-only build: compiles the engine into web/earth.js + web/earth.wasm as a
# modularized library (createEarthModule) for web/index.html. the native and
# standalone-emscripten targets were removed — see the root README's
# "stuff we've removed" section to dig them out of git history.
# bash, not sh: both this and config_emscripten.sh are bash (source,
# BASH_SOURCE), and under dash the sourcing fails quietly and the protobuf
# paths come out empty. -u turns that class of bug into an error.
set -eu
cd "$(dirname "$0")"
source config_emscripten.sh
echo build: weblib

# these inputs are effectively frozen, so they're only rebuilt when they
# change. `-nt` is also true when the target is missing, so a fresh checkout
# (ci, always) still does the full work. crn.o doesn't track crn's headers —
# delete it by hand on the rare occasion you touch them.
if [ proto/rocktree.proto -nt proto/rocktree.pb.h ]; then
	$EMSCRIPTEN_PROTOBUF_EXE --cpp_out=. proto/rocktree.proto
fi
# the generated code runs on the decode threads, so it gets -pthread like the
# engine and libprotobuf
if [ proto/rocktree.pb.cc -nt proto/rocktree.pb.o ]; then
	em++ -std=c++17 -O2 -pthread -c proto/rocktree.pb.cc -o proto/rocktree.pb.o \
		-I. -I$EMSCRIPTEN_PROTOBUF_SRC
fi
if [ crn/crn.cc -nt crn/crn.o ]; then
	(cd crn && emcc -std=c++14 -O2 -c crn.cc -w)
fi
# the stl export's alpha wrap (earth_stl.h): cgal, a minute to compile, so
# only when it changes. cgal's filtered predicates throw to fall back to
# exact arithmetic, so it needs real exceptions (and the link below with
# it); wasm can't set the fpu's rounding mode, which its interval arithmetic
# otherwise relies on; and boost's multiprecision stands in for gmp
if [ earth_stl.cpp -nt earth_stl.o ] || [ earth_stl.h -nt earth_stl.o ]; then
	em++ -std=c++17 -O2 -pthread -fwasm-exceptions -c earth_stl.cpp -o earth_stl.o \
		-I$EMSCRIPTEN_CGAL_INC -s USE_BOOST_HEADERS=1 \
		-DCGAL_ALWAYS_ROUND_TO_NEAREST -DCGAL_DISABLE_GMP -DNDEBUG
fi

mkdir -p web
cp coi-serviceworker.js web/
# em++, not emcc: from emscripten 6 emcc no longer links libc++ just because
# the input is a .cpp, and the link fails on ostream/locale symbols
# EARTH_PROFILE=1 keeps function names in the wasm, so a browser cpu profile
# (tools/loadprof.mjs --cpuprofile) names the engine's functions instead of
# showing wasm-function[1234]. a little bigger, so not the default
em++ earth_web.cpp -O2 -std=c++17 -Wno-deprecated-declarations -I. -Ideps/eigen \
	${EARTH_PROFILE:+--profiling-funcs} \
	-I$EMSCRIPTEN_PROTOBUF_SRC proto/rocktree.pb.o $EMSCRIPTEN_PROTOBUF_LIB crn/crn.o \
	earth_stl.o -fwasm-exceptions \
	-s MALLOC=mimalloc -Wno-pthreads-mem-growth \
	-s FETCH=1 -s USE_PTHREADS=1 \
	-s MAX_WEBGL_VERSION=2 -s MIN_WEBGL_VERSION=2 \
	-s INITIAL_MEMORY=536870912 -s ALLOW_MEMORY_GROWTH=1 -s MAXIMUM_MEMORY=4294967296 \
	-s PTHREAD_POOL_SIZE="'navigator.hardwareConcurrency'" \
	--bind -s MODULARIZE=1 -s EXPORT_NAME=createEarthModule \
	-o web/earth.js
