#!/bin/bash
# paths into ./deps, populated by ./setup.sh — no machine-local locations
DEPS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/deps"

EMSCRIPTEN_PROTOBUF_SRC="$DEPS/protobuf-3.21.12/src"
EMSCRIPTEN_PROTOBUF_LIB="$DEPS/protobuf-3.21.12/src/.libs/libprotobuf.a"
EMSCRIPTEN_PROTOBUF_EXE="$DEPS/protoc/bin/protoc"
