#!/bin/bash
# one-time dependency setup: fetches pinned releases from their official
# homes into ./deps (gitignored). after this, ./build.sh works. needs
# emscripten (emcc) on the PATH and curl.
set -euo pipefail
cd "$(dirname "$0")"

EIGEN_VERSION=3.3.7
PROTOBUF_VERSION=21.12 # protobuf's post-2022 versioning; cpp tarball says 3.21.12
CGAL_VERSION=6.2

mkdir -p deps

# --- eigen (header-only, just unpack) ---------------------------------------
if [ ! -e deps/eigen/Eigen/Dense ]; then
	echo "setup: eigen $EIGEN_VERSION"
	curl -fL "https://gitlab.com/libeigen/eigen/-/archive/$EIGEN_VERSION/eigen-$EIGEN_VERSION.tar.gz" | tar xz -C deps
	rm -rf deps/eigen
	mv "deps/eigen-$EIGEN_VERSION" deps/eigen
fi

# --- cgal (header-only, just unpack): the stl export's alpha wrap. the
# boost it needs is emscripten's own port (USE_BOOST_HEADERS in build.sh)
if [ ! -e deps/cgal/include/CGAL/alpha_wrap_3.h ]; then
	echo "setup: cgal $CGAL_VERSION"
	curl -fL "https://github.com/CGAL/cgal/releases/download/v$CGAL_VERSION/CGAL-$CGAL_VERSION-library.tar.xz" | tar xJ -C deps
	rm -rf deps/cgal
	mv "deps/CGAL-$CGAL_VERSION" deps/cgal
fi

# --- protoc (prebuilt release binary for the host) --------------------------
if [ ! -x deps/protoc/bin/protoc ]; then
	echo "setup: protoc $PROTOBUF_VERSION"
	case "$(uname -s)-$(uname -m)" in
		Darwin-*) PROTOC_ZIP="protoc-$PROTOBUF_VERSION-osx-universal_binary.zip" ;;
		Linux-x86_64) PROTOC_ZIP="protoc-$PROTOBUF_VERSION-linux-x86_64.zip" ;;
		Linux-aarch64) PROTOC_ZIP="protoc-$PROTOBUF_VERSION-linux-aarch_64.zip" ;;
		*) echo "setup: unsupported host $(uname -s)-$(uname -m)" >&2; exit 1 ;;
	esac
	curl -fLo deps/protoc.zip "https://github.com/protocolbuffers/protobuf/releases/download/v$PROTOBUF_VERSION/$PROTOC_ZIP"
	rm -rf deps/protoc
	unzip -q deps/protoc.zip -d deps/protoc
	rm deps/protoc.zip
fi

# --- libprotobuf compiled to wasm (the slow part, a few minutes) ------------
if [ ! -e "deps/protobuf-3.$PROTOBUF_VERSION/src/.libs/libprotobuf.a" ]; then
	echo "setup: libprotobuf $PROTOBUF_VERSION -> wasm"
	command -v emcc >/dev/null || { echo "setup: emcc not on PATH" >&2; exit 1; }
	if [ ! -e "deps/protobuf-3.$PROTOBUF_VERSION/configure" ]; then
		curl -fL "https://github.com/protocolbuffers/protobuf/releases/download/v$PROTOBUF_VERSION/protobuf-cpp-3.$PROTOBUF_VERSION.tar.gz" | tar xz -C deps
	fi
	cd "deps/protobuf-3.$PROTOBUF_VERSION"
	emconfigure ./configure --disable-shared --disable-maintainer-mode >configure.log 2>&1 \
		|| { tail -20 configure.log >&2; exit 1; }
	emmake make -C src -j"$(getconf _NPROCESSORS_ONLN)" libprotobuf.la >build.log 2>&1 \
		|| { tail -20 src/build.log build.log 2>/dev/null >&2; exit 1; }
	cd ../..
fi

echo "setup: done"
