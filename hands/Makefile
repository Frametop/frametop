# Hand tracking, built natively for aarch64 into build/:
#   make          fh-camd (the camera broker) and fh-hands (the tracker)
#   make check    builds and runs the C++ unit tests (tests/sides_test.cpp, tests/skeleton_test.cpp,
#                 tests/pinch_test.cpp, tests/flagbuf_test.cpp)
# The first build fetches ncnn's source (release NCNN_TAG, commit e54f7b1f88434e1d844ea0551b880a1cfb079ce1)
# and builds it into build/ncnn, which takes a few minutes. The download is the release's
# full-source zip, checked by NCNN_SHA256 (the digest GitHub lists for that asset). NCNN=DIR
# uses an ncnn install already built instead.
NCNN_TAG  = 20260526
NCNN_SHA256 = 754659d6fe65545cf2ef4483ffb84526fea631f8764c44b150f1601d0fb4004b
NCNN     ?= build/ncnn/install

# JsonCpp 1.9.6, pinned to the source archive and digest used by protobuf.
# Ubuntu's libjsoncpp-dev has headers but no static libjsoncpp.a on ARM64.
JSONCPP_COMMIT = 89e2973c754a9c02a49974d839779b151e95afd6
JSONCPP_SHA256 = 02f0804596c1e18c064d890ac9497fa17d585e822fcacf07ff8a8aa0b344a7bd
JSONCPP ?= build/jsoncpp/install
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-psabi
CXXFLAGS += -std=c++17 -fopenmp -I$(NCNN)/include/ncnn -I$(JSONCPP)/include
LDLIBS    = $(NCNN)/lib/libncnn.a $(JSONCPP)/lib/libjsoncpp.a -fopenmp -lpthread
# Runs directly on SteamOS, without the build host's C++/jsoncpp libraries.
LDFLAGS  ?= -static

CAMD  = camd/camd.c camd/tp.c camd/xrcams.c
TRACK = track/calib.cpp track/nets.cpp track/tracker.cpp track/io.cpp track/record.cpp track/pinch.cpp track/sides.cpp track/skeleton.cpp track/flagbuf.cpp
HDR   = $(wildcard track/*.h) camd/fhring.h include/fh_hands.h include/fh_gestures.h ../config/ft_conf.h

all: build/fh-camd build/fh-hands

build/fh-camd: $(CAMD) camd/tp.h camd/xrcams.h camd/fhring.h ../config/ft_conf.h
	@mkdir -p build
	$(CC) $(CFLAGS) -static -o $@ $(CAMD) -lm

build/fh-hands: track/main.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a $(JSONCPP)/lib/libjsoncpp.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ track/main.cpp $(TRACK) $(LDLIBS)

build/sides-test: tests/sides_test.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a $(JSONCPP)/lib/libjsoncpp.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/sides_test.cpp $(TRACK) $(LDLIBS)

build/skeleton-test: tests/skeleton_test.cpp track/skeleton.cpp track/skeleton.h track/geom.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/skeleton_test.cpp track/skeleton.cpp

build/pinch-test: tests/pinch_test.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a $(JSONCPP)/lib/libjsoncpp.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/pinch_test.cpp $(TRACK) $(LDLIBS)

build/flagbuf-test: tests/flagbuf_test.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a $(JSONCPP)/lib/libjsoncpp.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/flagbuf_test.cpp $(TRACK) $(LDLIBS)

build/shm-test: tests/shm_test.cpp include/fh_shm.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/shm_test.cpp

build/xrcams-test: tests/xrcams_test.c camd/xrcams.c camd/xrcams.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/xrcams_test.c

check: build/sides-test build/skeleton-test build/pinch-test build/flagbuf-test build/shm-test build/xrcams-test
	build/sides-test
	build/skeleton-test
	build/pinch-test
	build/flagbuf-test
	build/shm-test
	build/xrcams-test

# ncnn as frame-hands built it (the models were converted and quantized for it), minus its tools
build/ncnn/install/lib/libncnn.a:
	rm -rf build/ncnn && mkdir -p build/ncnn
	curl -fsSL -o build/ncnn/src.zip https://github.com/Tencent/ncnn/releases/download/$(NCNN_TAG)/ncnn-$(NCNN_TAG)-full-source.zip
	echo "$(NCNN_SHA256)  build/ncnn/src.zip" | sha256sum -c --quiet
	unzip -q build/ncnn/src.zip -d build/ncnn/src && rm build/ncnn/src.zip
	cmake -S build/ncnn/src -B build/ncnn/build -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_INSTALL_PREFIX=$(CURDIR)/build/ncnn/install -DCMAKE_INSTALL_LIBDIR=lib -DNCNN_VULKAN=OFF \
	  -DNCNN_OPENMP=ON -DNCNN_INT8=ON -DNCNN_SIMPLEOCV=ON -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF \
	  -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF -DNCNN_PYTHON=OFF > build/ncnn/cmake.log
	cmake --build build/ncnn/build --target install > build/ncnn/build.log

# Use one known static JsonCpp build, independent of distro dev packages.
build/jsoncpp/install/lib/libjsoncpp.a:
	rm -rf build/jsoncpp && mkdir -p build/jsoncpp/src
	curl -fL --retry 3 -o build/jsoncpp/src.zip https://github.com/open-source-parsers/jsoncpp/archive/$(JSONCPP_COMMIT).zip
	echo "$(JSONCPP_SHA256)  build/jsoncpp/src.zip" | sha256sum -c --quiet
	unzip -q build/jsoncpp/src.zip -d build/jsoncpp/src && rm build/jsoncpp/src.zip
	cmake -S build/jsoncpp/src/jsoncpp-$(JSONCPP_COMMIT) -B build/jsoncpp/build -G Ninja -Wno-dev \
	  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(CURDIR)/build/jsoncpp/install \
	  -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF -DBUILD_OBJECT_LIBS=OFF \
	  -DJSONCPP_WITH_TESTS=OFF -DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF
	cmake --build build/jsoncpp/build --target install

clean:
	rm -f build/fh-camd build/fh-hands build/sides-test build/skeleton-test build/pinch-test build/flagbuf-test build/shm-test build/xrcams-test

.PHONY: all check clean
