# Hand tracking, built into build/ (hands/build.sh runs this in the dev container):
#   make          ft-camd (camd/: runs on the host, so linked statically) and ft-hands (track/)
#   make tools    ft-handreplay and ft-ringplay, for recordings
#   make check    builds and runs the C++ unit tests (tests/sides_test.cpp, tests/skeleton_test.cpp,
#                 tests/pinch_test.cpp, tests/flagbuf_test.cpp)
# The first build fetches ncnn's source (release NCNN_TAG, commit e54f7b1f88434e1d844ea0551b880a1cfb079ce1)
# and builds it into build/ncnn, which takes a few minutes. The download is the release's
# full-source zip, checked by NCNN_SHA256 (the digest GitHub lists for that asset). NCNN=DIR
# uses an ncnn install already built instead.
NCNN_TAG  = 20260526
NCNN_SHA256 = 754659d6fe65545cf2ef4483ffb84526fea631f8764c44b150f1601d0fb4004b
NCNN     ?= build/ncnn/install
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-psabi
CXXFLAGS += -std=c++17 -fopenmp -I$(NCNN)/include/ncnn
LDLIBS    = $(NCNN)/lib/libncnn.a -ljsoncpp -fopenmp -lpthread
# The standalone Hand Recorder's release build passes LDFLAGS=-static (it runs on the host).
LDFLAGS  ?=

CAMD  = camd/camd.c camd/camstart.c camd/tp.c camd/xrcams.c
TRACK = track/calib.cpp track/nets.cpp track/tracker.cpp track/io.cpp track/record.cpp track/pinch.cpp track/sides.cpp track/skeleton.cpp track/flagbuf.cpp track/calibfile.cpp track/profile.cpp \
        track/npuheads.cpp track/npuclient.cpp track/npuserver.cpp track/npusession.cpp track/npulandmarks.cpp track/npurequests.cpp track/npudetector.cpp track/detrounds.cpp track/park.cpp track/pidfile.cpp
HDR   = $(wildcard track/*.h) camd/fhring.h include/fh_hands.h include/fh_gestures.h include/fh_calib.h include/fh_verdicts.h include/fh_shm.hpp ../conf/ft_conf.h

all: build/ft-camd build/ft-hands
tools: build/ft-handreplay build/ft-ringplay

build/ft-camd: $(CAMD) camd/camstart.h camd/tp.h camd/xrcams.h camd/fhring.h ../conf/ft_conf.h
	@mkdir -p build
	$(CC) $(CFLAGS) -static -o $@ $(CAMD) -lm

build/ft-hands: track/main.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ track/main.cpp $(TRACK) $(LDLIBS)

build/ft-handreplay: track/replay.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ track/replay.cpp $(TRACK) $(LDLIBS)

build/sides-test: tests/sides_test.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/sides_test.cpp $(TRACK) $(LDLIBS)

build/skeleton-test: tests/skeleton_test.cpp track/skeleton.cpp track/skeleton.h track/geom.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/skeleton_test.cpp track/skeleton.cpp

build/profile-test: tests/profile_test.cpp track/profile.cpp track/profile.h track/skeleton.cpp track/skeleton.h track/geom.h include/fh_verdicts.h include/fh_shm.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/profile_test.cpp track/profile.cpp track/skeleton.cpp

build/pinch-test: tests/pinch_test.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/pinch_test.cpp $(TRACK) $(LDLIBS)

build/flagbuf-test: tests/flagbuf_test.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/flagbuf_test.cpp $(TRACK) $(LDLIBS)

check: build/sides-test build/skeleton-test build/profile-test build/pinch-test build/flagbuf-test build/calibfile-test build/npuheads-test \
       build/npudetector-test build/npuclient-test build/npuserver-test build/npurequests-test build/trackerdet-test \
       build/cpus-test build/park-test
	build/cpus-test
	build/park-test
	build/npuserver-test
	build/npurequests-test
	build/npuheads-test
	build/npudetector-test
	build/npuclient-test
	build/trackerdet-test
	build/sides-test
	build/skeleton-test
	build/profile-test
	build/pinch-test
	build/flagbuf-test
	build/calibfile-test

build/park-test: tests/park_test.cpp track/park.cpp track/park.h track/pidfile.cpp track/pidfile.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/park_test.cpp track/park.cpp track/pidfile.cpp

build/cpus-test: tests/cpus_test.cpp track/cpus.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/cpus_test.cpp

build/npuheads-test: tests/npuheads_test.cpp track/npuheads.cpp track/npuheads.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/npuheads_test.cpp track/npuheads.cpp

build/npudetector-test: tests/npudetector_test.cpp track/npudetector.cpp track/npudetector.h $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/npudetector_test.cpp $(TRACK) $(LDLIBS)

build/trackerdet-test: tests/trackerdet_test.cpp $(TRACK) $(HDR) $(NCNN)/lib/libncnn.a
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ tests/trackerdet_test.cpp $(TRACK) $(LDLIBS)

build/npuclient-test: tests/npuclient_test.cpp track/npuclient.cpp track/npuclient.h track/npuserver.cpp track/npuserver.h track/npusession.cpp track/npusession.h track/npuheads.cpp track/npuheads.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/npuclient_test.cpp track/npuclient.cpp track/npuserver.cpp track/npusession.cpp track/npuheads.cpp -lpthread

build/npuserver-test: tests/npuserver_test.cpp track/npuserver.cpp track/npuserver.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/npuserver_test.cpp track/npuserver.cpp

build/npurequests-test: tests/npurequests_test.cpp track/npurequests.cpp track/npurequests.h track/npuclient.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/npurequests_test.cpp track/npurequests.cpp

build/calibfile-test: tests/calibfile_test.cpp track/calibfile.cpp track/calibfile.h track/calib.h include/fh_calib.h include/fh_shm.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/calibfile_test.cpp track/calibfile.cpp

build/ft-ringplay: track/ringplay.cpp track/record.h camd/fhring.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ track/ringplay.cpp

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

clean:
	rm -f build/ft-camd build/ft-hands build/ft-handreplay build/ft-ringplay build/sides-test build/skeleton-test build/profile-test build/pinch-test build/flagbuf-test build/npudetector-test build/npuclient-test build/trackerdet-test

.PHONY: all tools check clean
