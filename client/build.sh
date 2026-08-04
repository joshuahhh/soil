#!/bin/sh

if [ "$1" == "emscripten" ]; then
	source config_emscripten.sh
	echo build: emscripten
	pwd="$(pwd)" && cd .. && $EMSCRIPTEN_PROTOBUF_EXE --cpp_out=client proto/rocktree.proto && cd "$pwd"
	cd crn && emcc -std=c++14 -O2 -c crn.cc -w && cd ..

	emcc -Iinclude main.cpp -O2 -std=c++14 -I. -I./eigen/ \
		-I$EMSCRIPTEN_PROTOBUF_SRC $EMSCRIPTEN_PROTOBUF_LIB crn/crn.o \
		-s USE_SDL=2 -s FETCH=1 -s TOTAL_MEMORY=1073741824 -s USE_PTHREADS=1 \
		-s PTHREAD_POOL_SIZE="'navigator.hardwareConcurrency'" \
		--shell-file shell.html \
		-o main.html
else
	echo build: native
	pwd="$(pwd)" && cd .. && protoc --cpp_out=client proto/rocktree.proto && cd "$pwd"
	cd crn && g++ -std=c++14 -O2 -c crn.cc -w && cd ..

	CFLAGS="--std=c++17 -O2 -g -I. `pkg-config --cflags sdl2 protobuf` -I./eigen/"
	LDFLAGS="`pkg-config --libs sdl2 protobuf` crn/crn.o"
	if [ `uname` = "Darwin" ]; then
		if [ "$1" == "gl" ]; then
			# legacy OpenGL backend, kept for A/B comparison ("./build.sh gl")
			echo build: native gl
			CFLAGS="$CFLAGS -DEARTH_USE_GL `pkg-config --cflags glew`"
			LDFLAGS="$LDFLAGS `pkg-config --static --libs glew` -framework OpenGL"
			c++ $CFLAGS main.cpp $LDFLAGS -o main
		else
			# metal backend (default on macOS); the renderer header is
			# objective-c++, so compile the single TU as such
			echo build: native metal
			LDFLAGS="$LDFLAGS -framework Metal -framework QuartzCore -framework Foundation"
			c++ -fno-objc-arc $CFLAGS -x objective-c++ main.cpp -x none $LDFLAGS -o main
		fi
	else
		CFLAGS="$CFLAGS -Igl2/include"
		LDFLAGS="$LDFLAGS -lGL -lm -ldl"
		c++ $CFLAGS main.cpp $LDFLAGS -o main
	fi
fi
