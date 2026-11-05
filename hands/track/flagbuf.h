// Flagging tracking failures (frame-hands notes/flag-failures.md has the plan and the format): a
// rolling buffer, in RAM, of what's needed to relabel and diagnose the last few seconds of
// tracking, and on a flag (`ft-handsctl flag [note]`, or a voice command through it), the buffer
// and the next couple of seconds written to a folder for the offline tools (frame-hands'
// train/flags).
//
// Per processed set it keeps, per view the tracker used, a mono crop around the tracker's
// landmark crop: its centre, `scale` times its size, axis aligned (so it covers the rotated crop
// with room to spare), downscaled to at most `crop_px`; with the crop's place in the image, so
// labels made on it lift back to the full image and triangulate. A view the tracker lost is still
// cropped where it was for `ghost_s` (a "ghost"), so the teacher can say whether the hand was there.
// And ft-hands' output for the set (its hands and gestures, as JSON) and each view's crop and
// landmarks. Nothing is written until a flag.
//
// The crops live in fixed slots of one arena (`max_bytes`), taken and given back through a free
// list; pages are only touched as slots are used. Sets older than `before_s` are let go; when the
// slots run out, the oldest set goes, and while a flag is waiting for its last seconds its sets
// are held, so a crop that finds no free slot then is dropped (counted). A flag's sets leave the
// buffer when it's complete, and a thread of their own writes them (at low priority, off the
// tracking cores), giving the slots back as it goes.
#pragma once

#include "calib.h"
#include "tracker.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Pinch;
class Grip;

struct FlagConfig {
    double before_s = 12;    // kept before a flag
    double after_s = 2;      // and taken after it
    double scale = 1.6;      // the crop's side over the tracker's crop's
    double ghost_s = 1.0;    // a lost view is still cropped where it was this long
    int crop_px = 224;       // a crop's side at most (larger ones are downscaled)
    int max_ghosts = 4;      // ghost crops per set at most
    size_t max_bytes = size_t(80) << 20;   // the crops' arena
    std::vector<int> writer_cpus = {0, 1, 2, 3, 4};   // where the writer runs (empty: anywhere)
};

// One crop's place: image pixel (centres at integers) of crop pixel (u, v) is
// (x0 + (u + 0.5) * scale - 0.5, y0 + (v + 0.5) * scale - 0.5). Outside the image is 0.
struct CropGeom {
    double x0 = 0, y0 = 0, scale = 1;
    int w = 0, h = 0;
};

// The square of side `side` centred on `center`, axis aligned, downscaled to at most max_px a side,
// into out (w * h bytes). Downscaled by more than 1.25, each pixel averages a 2 x 2 grid of taps
// across its footprint; else the nearest pixel.
CropGeom crop_square(const Image &img, V2 center, double side, int max_px, uint8_t *out);

// A flag's request: where it goes and what the manifest says about it besides the buffer's own
// (`fields`: JSON members, "\"key\": value, ..." without braces, may be empty).
struct FlagRequest {
    std::string dir;          // exists, or is made
    std::string fields;
};

class FlagBuffer {
public:
    explicit FlagBuffer(const FlagConfig &cfg = {});
    ~FlagBuffer();   // waits for the writer
    const FlagConfig &config() const { return cfg_; }

    // The manifest's static parts, set once: the cameras (cameras_json) and ft-hands' settings
    // (a JSON object).
    void set_static(const std::string &cameras_json, const std::string &settings_json) {
        cameras_json_ = cameras_json, settings_json_ = settings_json;
    }
    // After each processed set: its capture time (t_raw: the cameras' clock, CLOCK_MONOTONIC_RAW;
    // t_mono: CLOCK_MONOTONIC), its index in a recording (-1 live), the images the tracker had, its
    // views this step (Tracker::views_now) and the set's output (hands_json: JSON members
    // "\"hands\": [...], \"gestures\": {...}", see output_json). Completes a waiting flag once
    // a set comes after_s past it.
    void add(int64_t t_raw, int64_t t_mono, int rec_set, const std::map<std::string, Image> &images,
             const std::vector<Seen> &views, const std::string &hands_json);
    // A flag at t_raw (the newest set's time, or now on that clock): keeps from before_s before it
    // and completes after_s after. False if a flag is already waiting (this one's note joins it:
    // `extra_note`) or the request is bad.
    bool flag(const FlagRequest &req, int64_t t_raw, const std::string &extra_note = "");
    // Completes a waiting flag if its time is up by the clock (no sets come while parked): call
    // now and then with the cameras' clock's now.
    void tick(int64_t t_raw_now);
    // Completes a waiting flag with what it has (stopping).
    void finish();
    bool waiting() const { return pending_; }
    // Waits for the writer.
    void wait();

    struct Stats {
        size_t sets = 0, crops = 0, ghosts = 0;
        size_t slots_used = 0, slots = 0;
        size_t dropped = 0;           // crops with no free slot
        size_t flags = 0;             // written
        double crop_us = 0;           // summed crop time, microseconds
        size_t crop_calls = 0;
        std::string last_dir, last_error;
    };
    Stats stats() const;
    // RAM the buffer holds now: the slots in use (their pages) and the sets' records.
    size_t bytes() const;

private:
    struct Crop {
        std::string cam;
        int hand = 0;
        bool ghost = false;
        CropGeom g;
        int slot = -1;
    };
    struct View {
        std::string cam;
        int hand = 0;
        double roi[4] = {};
        double presence = 0, right = 0;
        float lm[42] = {};
        int crop = -1;   // into the set's crops
    };
    struct Set {
        int64_t t_raw = 0, t_mono = 0;
        int rec_set = -1;
        std::string out;
        std::vector<View> views;
        std::vector<Crop> crops;
        bool held = false;
    };
    struct Dump {
        FlagRequest req;
        std::vector<Set> sets;
        int64_t flag_t = 0;
        double first_unix = 0;
        size_t dropped = 0;
    };
    int take_slot();
    void give_back(const std::vector<Crop> &crops);
    void evict_front();
    void complete();
    void write(Dump d);
    uint8_t *slot_ptr(int slot) { return arena_.get() + size_t(slot) * slot_bytes_; }

    FlagConfig cfg_;
    std::string cameras_json_ = "{}", settings_json_ = "{}";
    size_t slot_bytes_ = 0;
    int nslots_ = 0;
    std::unique_ptr<uint8_t[]> arena_;   // made at the first set; untouched pages cost nothing
    mutable std::mutex mu_;              // free_ and the stats the writer changes
    std::vector<int> free_;
    std::deque<Set> sets_;
    // where each camera's view of each hand last was, for ghosts
    struct Last {
        Roi roi;
        int64_t t = 0;
    };
    std::map<std::pair<std::string, int>, Last> last_;
    bool pending_ = false;
    FlagRequest req_;
    int64_t flag_t_ = 0;
    size_t dropped_at_flag_ = 0;
    std::string extra_notes_;
    std::thread writer_;
    Stats stats_;
};

// The calibrated cameras as the manifest has them (frame-hands notes/flag-failures.md).
std::string cameras_json(const std::map<std::string, Camera> &cams);
// A set's output for FlagBuffer::add: the hands (id, side, scores, measured and published points)
// and, if given, the gestures' state per side.
std::string output_json(const std::vector<const Hand *> &hands, const Pinch *pinch, const Pinch *middle, const Grip *grip);
// s as a JSON string ("..." with escapes); "null" when null_if_empty and s is empty.
std::string json_string(const std::string &s, bool null_if_empty = false);
