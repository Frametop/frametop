// MediaPipe's palm detector and hand landmark model on ncnn. A crop is a square region of a camera image: centre and size in
// pixels, and a rotation that turns the crop's "up" toward the image direction
// (sin r, -cos r). Crops are contrast-equalized (CLAHE) before the models see them.
// Everything here may run on several threads at once.
#pragma once

#include "geom.h"

#include <net.h>

#include <cstdint>
#include <string>
#include <vector>

struct Image {
    const uint8_t *data = nullptr;
    int width = 0, height = 0, stride = 0;
};

struct Roi {
    V2 center{};
    double size = 0, rotation = 0;
};

struct Palm {
    V2 center{}, size{};
    V2 kp[7]{};
    double score = 0;
    Roi roi() const;   // MediaPipe's hand crop for this palm
};

struct Landmarks {
    V2 pts[21]{};           // image pixels
    double world[21][3]{};  // MediaPipe's metric landmarks, hand-centred
    double presence = 0, right = 0;
    Roi next_roi() const;   // MediaPipe's crop to track the hand in the next frame
};

Roi roi_from_points(const V2 *pts21);

// How crops are contrast-equalized before the models see them.
struct Contrast {
    enum Mode { Clahe, None, Stretch } mode = Clahe;
    double clip = 2.0;   // Clahe: OpenCV's clip limit (4x4 tiles)
    // "clahe:2", "none", "stretch" (1st..99th percentile to 0..255)
    static bool parse(const std::string &s, Contrast &out);
    // "PALM/HAND" (each as above), or one for both
    static bool parse_pair(const std::string &s, Contrast &palm, Contrast &hand);
};

class Nets {
public:
    // Loads <dir>/palm.ncnn.* and <dir>/hand.ncnn.*, or the -int8 variants.
    bool load(const std::string &dir, bool int8, std::string &err);
    std::vector<Palm> palms(const Image &img, V2 center, double size, double rotation) const;
    Landmarks landmarks(const Image &img, const Roi &roi) const;
    // The landmark model's crop for roi as kHandCrop x kHandCrop gray bytes, equalized as landmarks() does, and
    // its crop-to-image matrix (for crop_to_image): what another landmark backend (the NPU's) reads.
    static constexpr int kHandCrop = 224;
    void hand_crop(const Image &img, const Roi &roi, uint8_t *out, float tm[6]) const;
    static V2 crop_to_image(const float tm[6], double x, double y);
    // The same size of crop equalized like the palm search's crops instead: the NPU detector's input (npu-hands
    // train/student/det.py: a camera image's tiles).
    void palm_crop(const Image &img, const Roi &roi, uint8_t *out, float tm[6]) const;
    // Before any palms()/landmarks(): how the palm search's and the landmark model's crops
    // are equalized.
    void set_contrast(const Contrast &palm, const Contrast &hand) { palm_contrast_ = palm, hand_contrast_ = hand; }

private:
    Contrast palm_contrast_, hand_contrast_;
    ncnn::Net palm_, hand_;
    std::vector<V2> anchors_;
};
