// track/tracker.cpp with a palm detector (DetectorRounds): palms collected for a camera whose frame is missing
// from this step are not searched (they used to reach images.at() and throw), and the next camera's palms still
// reach the landmark backend. Fakes only: no models, no ft-npud.
//   build/trackerdet-test
#include "../track/landmarkbatch.h"
#include "../track/palmdetector.h"
#include "../track/tracker.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures; \
    } while (0)

namespace {

// covers every camera; one palm per request, in the middle of the image
struct FakeDetector : PalmDetector {
    std::vector<std::string> posted;
    std::string camera;
    bool covers(const std::string &) const override { return true; }
    bool post(const Image &, const std::string &cam) override {
        posted.push_back(cam), camera = cam, pending_ = true;
        return true;
    }
    bool collect(std::string &cam, std::vector<Palm> &out) override {
        pending_ = false;
        cam = camera;
        Palm p;
        p.center = {320, 240}, p.size = {80, 80}, p.score = 0.9;
        for (V2 &k : p.kp) k = {320, 240};
        p.kp[2] = {320, 200};
        out = {p};
        return true;
    }
};

// records the images it was asked about; finds no hand there
struct FakeBatch : LandmarkBatch {
    std::vector<const uint8_t *> seen;   // each image's pixels
    bool run(const std::vector<const Image *> &images, const std::vector<Roi> &rois,
             std::vector<Landmarks> &out) override {
        for (const Image *im : images) seen.push_back(im->data);
        out.assign(rois.size(), Landmarks{});
        return true;
    }
};

Camera cam(const std::string &name) {
    Camera c;
    c.name = name, c.width = 640, c.height = 480, c.fx = c.fy = 300, c.cx = 320, c.cy = 240;
    for (int i = 0; i < 3; ++i) c.R[i][i] = 1;
    return c;
}

}  // namespace

int main() {
    const std::map<std::string, Camera> cams = {{"a", cam("a")}, {"b", cam("b")}};
    Nets nets;   // unloaded: every camera is the detector's, so no CPU tile runs
    Pool pool(1, {0});
    Tracker tr(cams, nets, pool);
    FakeDetector det;
    FakeBatch batch;
    tr.set_palm_detector(&det);
    tr.set_landmark_batch(&batch);
    std::vector<uint8_t> pa(640 * 480, 0), pb(640 * 480, 0);
    const Image ia{pa.data(), 640, 480, 640}, ib{pb.data(), 640, 480, 640};
    const int64_t t0 = 10000000000, step_ns = 1000000000 / 15;   // t0: a search is due (none ran yet)
    try {
        tr.step({{"a", ia}, {"b", ib}}, t0);          // a search starts a round: a posted at the end
        CHECK(det.posted == std::vector<std::string>{"a"});
        tr.step({{"b", ib}}, t0 + step_ns);             // a's palms arrive, but a's frame is missing; b posted
        CHECK(batch.seen.empty());
        CHECK((det.posted == std::vector<std::string>{"a", "b"}));
        tr.step({{"a", ia}, {"b", ib}}, t0 + 2 * step_ns); // b's palms: searched in b's image
        CHECK(batch.seen.size() == 1 && batch.seen[0] == pb.data());
    } catch (const std::exception &e) {
        std::printf("FAIL: step threw %s\n", e.what());
        ++failures;
    }
    std::printf(failures ? "trackerdet: %d failures\n" : "trackerdet: ok\n", failures);
    return failures != 0;
}
