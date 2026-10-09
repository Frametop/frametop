// ft-handreplay: run a recording (ft-hands --record) through the tracker offline, with the
// live scheduling, and report how well it kept the hands.
//
//   ft-handreplay DIR [--oracle N] [--slow F] [--timeline FILE] [--threads N] [--models DIR]
//             [--from S] [--to S] [--contrast MODE|PALM/HAND] (clahe[:CLIP], none, stretch)
//             [--sides file|0|1|auto]
//
// --oracle N: every N-th set, also search every tile of every camera (slow), to see
//             which hands were there to find. Compares that with what the tracker had.
// --slow F:   the live tracker skips the sets that arrive while it's busy; replay takes
//             each step's time here times F as the busy time (the headset is busier live).
// --cost:     instead of timing the steps, charge each round of model calls what it
//             typically costs live (10 ms landmarks, 18 ms palms): repeatable results.
// --cost-hand MS, --cost-palm MS: those per-round costs (with --cost). The 2026-10-08 live probe
//             (CPUs 0,1,4 beside SteamVR, 3 threads) measured about 14 and 52.
// --timeline: per processed set, a line per hand (time, id, side, views, wrist) and per view
//             (hand, camera, presence, next crop, set index).
// --keep-presence P: landmark presence a tracked view needs to stay (default 0.5, as new ones).
// --reacquire S, --reacquire-tiles N, --reacquire-fast: aim the palm search at hands lost within S s
//             (Tracker::set_reacquire; default off, tiles 2).
// --near F: Tracker::set_near (0.5). --hold S: Tracker::set_hold (0.3). --search-budget N: palm tiles per round with one hand tracked (2).
// --drop-every S: the re-acquire test. Every S s, while two hands are tracked, one of them (left and
//             right in turn) is made invisible for 0.35 s (Tracker::blind: just past the 0.3 s the tracker
//             holds a lost hand), and the report says how long it then took to have two hands again:
//             the time to find a hand that never left the view.
// --pair-sides: two hands seen at once get one left and one right (Tracker::set_pair_sides).
// --misread-guard: the tracker's guards for fine-tuned landmark models (Tracker::set_misread_guard).
// --pinch-begin M, --pinch-end M, --pinch-triangulated, --pinch-palm-down MAX, --pinch-dip 0|1: the pinch
//             detector (track/pinch.h; --pinch-dip 1: PinchParams::dip, as HANDS_PINCH_DIP=1);
//             the timeline gets its begin/end/lost events and both distance measures per set.
// --grip-begin R, --grip-end R: the grip detector (a closed hand; track/pinch.h); the timeline
//             gets its events and each side's finger curl per set.
// --cams mono|color|all: which cameras to track with (default mono). color and all need a
//             recording made with ft-camd --with-color; --color-left NODE (color_video0 or
//             color_video3) and --color-crop subtract|none say how its calibration maps
//             (tools/check_color.py).
// --contrast: how the palm search's and the landmark model's crops are equalized
//             (default clahe:2/none, as ft-hands).
// --poses FILE: per processed set, a line per hand: time, id, the model's left/right call,
//             views, hand scale, then its 21 world landmarks (the model's own 3D pose, averaged
//             over its views, times the scale; metres, hand-centred) and its 21 published
//             points (head frame). For studying gestures (pinch against typing, a fist).
// --sides file|0|1|auto: the side cameras' names as DIR/sides.json says they should be (file, the
//             default: ft-hands writes one with every recording; hands/rec/sides.py has the rule;
//             without one, as recorded), as recorded (0), exchanged (1), or as ft-hands' auto
//             decides them from the recorded names (track/sides.h): exchanged from the set it decides on,
//             the tracker's views moving with their images (Tracker::exchange). The side check runs in every mode, and the report
//             says what it found and when: seconds into the recording, and after how long of
//             hands (from the first set with a hand in view).
// --no-skeleton: the tracker without the skeleton fit (track/skeleton.h). With it (the
//             default) the report ends with the skeleton's measures (track/skeval.h): fit time
//             and residual, the bone-length calibration, bone-length spread, jitter, fingertip
//             lag and the elbow, each for the measured, One Euro filtered and fitted landmarks.
// --skel NAME=VALUE: a skeleton setting (skel::Params: pos_min, pos_beta, rot_min, rot_beta,
//             ang_min, ang_beta, d_cutoff, glitch, depth_ratio, limits, iterations), repeatable.
// --labels FILE: frame-hands' auto-labels (labels.jsonl) for this recording: the landmarks'
//             distance to them, measured, One Euro and fitted.
// --hmd-poses FILE: a hand-recorder take's poses.jsonl: the arm model's elbow with gravity's up
//             against the head's up.
// --publish-dump FILE: what ft-hands would have published after each processed set, for
//             Frametop's offline tests (screens/tests): a header (FHDUMP02, then the sizes of
//             fh_hands_v2_t and fh_gestures_t as two uint32), then per set the hands file's and
//             the gestures file's bytes and the set's dequeue time (int64, CLOCK_MONOTONIC: the
//             clock of a hand-recorder take's poses.jsonl; capture_ns is the cameras' clock).
//             publish_ns is the set's time plus the step's busy time.
// --skeleton-dump FILE: with --labels, a line per hand matched to a label: time, id, stereo, then
//             the measured, fitted (unfiltered) and labelled landmarks.
// --pinch-fitted: also run a second pinch detector on the fitted tips, and report its count.
// The middle pinch (thumb to middle fingertip) runs beside the pinch as in ft-hands; the timeline
// gets its events as "pinch2".
// --ray FILE: per processed set, a line per hand with what the pointer helper's hand ray reads
//             (hands/include/fh_ray.h; pointer/tools/rayeval): the set's capture time (ns,
//             CLOCK_MONOTONIC_RAW as recorded), id, side (L/R), stereo, fitted, its side's pinch
//             down, grip down, and pinch begun in this set (0/1 each), then the 21 published points
//             (the fit's when it has one; head frame), the wrist's frame as a quaternion (x y z w;
//             0 0 0 1 without a fit) and the elbow.
// --tips FILE: per processed set, a line per hand for frame-hands' train/taps/tapeval.py (the pinch
//             distance against auto-labels): "H", the set's capture time (ns, the labels' t_ns), id, side
//             (L/R), views, scale, the pinch detector's thumb-index measures (world landmarks as Pinch
//             averages them, triangulated, fitted; -1 none), the same for thumb-middle, the palm normal
//             (smoothed points, pointing out of the palm) and centre, then thumb tip, index tip and
//             middle tip as measured, smoothed and fitted (head frame), then per view: camera, presence,
//             and the 2D thumb, index and middle tips (px). Pinch events go in as "E time side
//             begin|end|lost distance" (side 0 left, 1 right; "E2" for the middle pinch), and each
//             processed set's time as "S time".
// --rate HZ:  process sets at a fixed rate (the live tracker adapts: 5 Hz with no hands, 15 still, 30
//             fast or while a pinch is down or closing). --want-hz HZ: as a reader's want-hz (ft-screens
//             near a panel): the live rule, but never slower than HZ.
// --depth FILE: per processed set, a line per hand for tools/depth_report.py: its views'
//             cameras, triangulation residual, hand scale, measured and published palm, and
//             each view's one-view palm (Tracker::single_view at the hand's scale). The
//             header has each camera's centre and focal length.
#include "io.h"
#include "pinch.h"
#include "record.h"
#include "sides.h"
#include "skeval.h"
#include "tracker.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

struct Track {
    double first = 0, last = 0;
    int sets = 0, left = 0;
    // the last two palm positions (raw, smoothed) and times, for the jitter measure
    V3 raw[2]{}, sm[2]{};
    double t[2]{};
    int line = 0;   // updates on the current unbroken run
};

V3 cross3(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argv[1][0] == '-') {
        std::printf("usage: %s DIR [--oracle N] [--slow F] [--timeline FILE] [--threads N] [--models DIR] [--from S] [--to S]\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];
    int oracle = 0, threads = 2;
    double slow = 1.0, from = 0, to = 1e9;
    bool cost = false;
    double cost_hand = 10, cost_palm = 18;
    Contrast palm_contrast, hand_contrast{Contrast::None};   // as ft-hands's
    double keep_presence = 0.5;   // landmark presence a tracked view needs to stay
    bool misread_guard = false;
    double reacquire = 0;
    int reacquire_tiles = 2;
    bool reacquire_fast = false, pair_sides = false;
    double drop_every = 0;   // seconds; 0: off
    double reacquire_clear = 0.12;
    double near_frac = 0.5, hold = 0.3;
    int search_budget1 = 2;
    bool skeleton = true, pinch_fitted = false;
    skel::Params skel_params;
    std::string labels, hmd_poses, skeleton_dump, publish_dump;
    PinchParams pinch_params;
    GripParams grip_params;
    std::string use = "mono", color_left = "color_video0", color_crop = "subtract", sides = "file";
    std::string ray_out, tips_out;
    double fixed_rate = 0, want_hz = 0;
    std::string timeline, depth, poses, models = std::string(argv[0]).substr(0, std::string(argv[0]).rfind('/') + 1) + "../models/ncnn";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--oracle" && more) oracle = std::atoi(argv[++i]);
        else if (a == "--slow" && more) slow = std::atof(argv[++i]);
        else if (a == "--timeline" && more) timeline = argv[++i];
        else if (a == "--depth" && more) depth = argv[++i];
        else if (a == "--poses" && more) poses = argv[++i];
        else if (a == "--ray" && more) ray_out = argv[++i];
        else if (a == "--tips" && more) tips_out = argv[++i];
        else if (a == "--rate" && more) fixed_rate = std::atof(argv[++i]);
        else if (a == "--want-hz" && more) want_hz = std::min(30.0, std::atof(argv[++i]));
        else if (a == "--threads" && more) threads = std::atoi(argv[++i]);
        else if (a == "--models" && more) models = argv[++i];
        else if (a == "--cost") cost = true;
        else if (a == "--cost-hand" && more) cost_hand = std::atof(argv[++i]);
        else if (a == "--cost-palm" && more) cost_palm = std::atof(argv[++i]);
        else if (a == "--keep-presence" && more) keep_presence = std::atof(argv[++i]);
        else if (a == "--misread-guard") misread_guard = true;
        else if (a == "--reacquire" && more) reacquire = std::atof(argv[++i]);
        else if (a == "--reacquire-tiles" && more) reacquire_tiles = std::atoi(argv[++i]);
        else if (a == "--reacquire-fast") reacquire_fast = true;
        else if (a == "--reacquire-clear" && more) reacquire_clear = std::atof(argv[++i]);
        else if (a == "--pair-sides") pair_sides = true;
        else if (a == "--drop-every" && more) drop_every = std::atof(argv[++i]);
        else if (a == "--near" && more) near_frac = std::atof(argv[++i]);
        else if (a == "--hold" && more) hold = std::atof(argv[++i]);
        else if (a == "--search-budget" && more) search_budget1 = std::atoi(argv[++i]);
        else if (a == "--no-skeleton") skeleton = false;
        else if (a == "--pinch-fitted") pinch_fitted = true;
        else if (a == "--labels" && more) labels = argv[++i];
        else if (a == "--hmd-poses" && more) hmd_poses = argv[++i];
        else if (a == "--skeleton-dump" && more) skeleton_dump = argv[++i];
        else if (a == "--publish-dump" && more) publish_dump = argv[++i];
        else if (a == "--skel" && more) {
            const std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            const std::string k = kv.substr(0, eq);
            const double v = eq == std::string::npos ? NAN : std::atof(kv.c_str() + eq + 1);
            double *f = k == "pos_min" ? &skel_params.pos_min : k == "pos_beta" ? &skel_params.pos_beta
                      : k == "rot_min" ? &skel_params.rot_min : k == "rot_beta" ? &skel_params.rot_beta
                      : k == "ang_min" ? &skel_params.ang_min : k == "ang_beta" ? &skel_params.ang_beta
                      : k == "d_cutoff" ? &skel_params.d_cutoff : k == "limits" ? &skel_params.limits
                      : k == "depth_ratio" ? &skel_params.depth_ratio
                      : k == "glitch" ? &skel_params.glitch
                      : k == "warm" ? &skel_params.warm : nullptr;
            if (k == "iterations") skel_params.iterations = int(v);
            else if (f && v == v) *f = v;
            else return std::fprintf(stderr, "--skel NAME=VALUE: %s?\n", kv.c_str()), 1;
        }
        else if (a == "--cams" && more) use = argv[++i];
        else if (a == "--sides" && more) sides = argv[++i];
        else if (a == "--pinch-begin" && more) pinch_params.begin_m = std::atof(argv[++i]);
        else if (a == "--pinch-end" && more) pinch_params.end_m = std::atof(argv[++i]);
        else if (a == "--pinch-triangulated") pinch_params.triangulated = true;
        else if (a == "--pinch-palm-down" && more) pinch_params.palm_down_max = std::atof(argv[++i]);
        else if (a == "--pinch-dip" && more) pinch_params.dip = std::atoi(argv[++i]) != 0;
        else if (a == "--grip-begin" && more) grip_params.begin = std::atof(argv[++i]);
        else if (a == "--grip-end" && more) grip_params.end = std::atof(argv[++i]);
        else if (a == "--color-left" && more) color_left = argv[++i];
        else if (a == "--color-crop" && more) color_crop = argv[++i];
        else if (a == "--contrast" && more) {
            if (!Contrast::parse_pair(argv[++i], palm_contrast, hand_contrast))
                return std::fprintf(stderr, "--contrast MODE or PALM/HAND, each clahe[:CLIP]|none|stretch\n"), 1;
        }
        else if (a == "--from" && more) from = std::atof(argv[++i]);
        else if (a == "--to" && more) to = std::atof(argv[++i]);
        else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 1;
    }
    std::string err;
    std::map<std::string, Camera> calib;
    Nets nets;
    SetReader in;
    if (!load_calibration(calib, err) || !nets.load(models, false, err) || !in.open(dir, err))
        return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    nets.set_contrast(palm_contrast, hand_contrast);
    FILE *tl = timeline.empty() ? nullptr : std::fopen(timeline.c_str(), "w");

    std::vector<fh_set_cam_t> cams;
    std::vector<std::vector<uint8_t>> px;
    if (!in.next(cams, px)) return std::fprintf(stderr, "%s: no sets\n", dir.c_str()), 1;
    if (use != "mono" && use != "color" && use != "all") return std::fprintf(stderr, "--cams mono|color|all\n"), 1;
    if (sides != "file" && sides != "0" && sides != "1" && sides != "auto")
        return std::fprintf(stderr, "--sides file|0|1|auto\n"), 1;
    // --sides file: from which set on the recorded names need exchanging (hands/rec/sides.py)
    std::vector<std::pair<int, bool>> file_runs = {{0, false}};
    if (sides == "file") {
        std::ifstream f(dir + "/sides.json");
        Json::Value v;
        Json::CharReaderBuilder b;
        std::string e;
        if (f && Json::parseFromStream(b, f, &v, &e) && v["swapped"].isBool()) {
            file_runs.clear();
            for (const Json::Value &r : v["names_swapped"])
                file_runs.push_back({r[0].asInt(), r[1].asBool() != v["swapped"].asBool()});
            if (file_runs.empty()) file_runs = {{0, v["swapped"].asBool()}};
            std::printf("side cameras: %s.json says swapped %s (%s)\n", (dir + "/sides").c_str(),
                        v["swapped"].asBool() ? "true" : "false", v["decided_by"].asString().c_str());
        }
    }
    if (use != "mono") {
        std::vector<std::string> nodes;
        for (auto &c : cams)
            if (std::string(c.name).rfind("color_video", 0) == 0) nodes.push_back(c.name);
        if (nodes.size() != 2) return std::fprintf(stderr, "%s: no color cameras (ft-camd --with-color)\n", dir.c_str()), 1;
        const std::string right = nodes[0] == color_left ? nodes[1] : nodes[0];
        if (!load_color_calibration(calib, color_left, right, color_crop == "subtract", 2, err))
            return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    }
    std::map<std::string, Camera> used;
    for (auto &c : cams) {
        const bool color = std::string(c.name).rfind("color_", 0) == 0;
        if (calib.count(c.name) && (use == "all" || color == (use == "color"))) used[c.name] = calib[c.name];
    }
    Pool pool(threads, {2, 3, 4});
    Tracker tracker(used, nets, pool);
    // The side cameras (see the top): names_swapped exchanges slam_left's and slam_right's names
    // as sets are read.
    bool names_swapped = sides == "1";
    std::map<std::string, Camera> mono;
    for (auto &[name, c] : used)
        if (name.rfind("color_", 0) != 0) mono[name] = c;
    SideCheck side_check(mono);
    bool checking = side_check.usable();
    int side_round = 0;
    double first_hand_ts = -1;
    std::string side_report;
    auto rename = [&](const std::string &n) -> std::string {
        if (!names_swapped) return n;
        return n == "slam_left" ? "slam_right" : n == "slam_right" ? "slam_left" : n;
    };
    tracker.set_keep_presence(keep_presence);
    tracker.set_misread_guard(misread_guard);
    tracker.set_reacquire(reacquire, reacquire_tiles, reacquire_fast, reacquire_clear);
    tracker.set_pair_sides(pair_sides);
    tracker.set_near(near_frac);
    tracker.set_hold(hold);
    tracker.set_search_budget(3, search_budget1);
    std::vector<std::string> trace;
    if (tl) {
        tracker.set_trace(&trace);
        for (const std::string &line : tracker.describe_tiles()) std::fprintf(tl, "# %s\n", line.c_str());
    }
    tracker.set_skeleton(skeleton);
    tracker.skeleton().set_params(skel_params);
    skeval::Eval eval;
    eval.dump = skeleton_dump;
    if (!labels.empty() && !eval.load_labels(labels)) return std::fprintf(stderr, "%s: can't read it\n", labels.c_str()), 1;
    std::vector<std::pair<int64_t, int>> label_hits;   // processed sets' times
    std::vector<std::pair<int64_t, V3>> ups;            // the head's up (head frame) by CLOCK_MONOTONIC
    if (!hmd_poses.empty()) {
        std::ifstream f(hmd_poses);
        std::string line;
        Json::CharReaderBuilder b;
        while (std::getline(f, line)) {
            Json::Value v;
            std::string e;
            std::istringstream in(line);
            if (!Json::parseFromStream(b, in, &v, &e) || !v["hmd"]["ok"].asBool() || v["hmd"]["m"].size() != 12) continue;
            const Json::Value &m = v["hmd"]["m"];   // 3x4, head -> room: up in the head frame is its second row
            ups.push_back({v["t"].asInt64(), unit(V3{m[4].asDouble(), m[5].asDouble(), m[6].asDouble()})});
        }
        if (ups.empty()) return std::fprintf(stderr, "%s: no HMD poses\n", hmd_poses.c_str()), 1;
    }
    FILE *dp = depth.empty() ? nullptr : std::fopen(depth.c_str(), "w");
    FILE *pp = poses.empty() ? nullptr : std::fopen(poses.c_str(), "w");
    FILE *rp = ray_out.empty() ? nullptr : std::fopen(ray_out.c_str(), "w");
    FILE *tp = tips_out.empty() ? nullptr : std::fopen(tips_out.c_str(), "w");
    if (tp) std::fprintf(tp, "# tips v1: H t id side views scale world tri fit world2 tri2 fit2 normal3 palm3 "
                             "thumb/index/middle x (measured, smoothed, fitted) x xyz, then cam presence 6 px\n");
    if (dp)
        for (auto &[name, c] : used)
            std::fprintf(dp, "# cam %s %.4f %.4f %.4f %.1f\n", name.c_str(), c.origin[0], c.origin[1], c.origin[2], c.fx);

    uint64_t t0 = 0, busy_until = 0, next_ns = 0, t_prev = 0;
    int index = -1;   // of the set in the recording
    int nsets = 0, processed = 0, left = 0, right = 0, both = 0, hist[3] = {};
    std::map<int, Track> tracks;
    std::vector<const Hand *> last_out;
    // oracle: sets where a side's hand was findable, and where the tracker had it then
    int o_sets = 0, o_left = 0, o_right = 0, o_left_hit = 0, o_right_hit = 0, o_left_extra = 0, o_right_extra = 0;
    std::map<std::string, int> o_by_cam;
    // label-free: per oracle set, hands the full search found (distinct left/right calls, at most 2)
    // against hands the tracker published
    int o_hands = 0, o_missed = 0;
    // the re-acquire test
    double drop_next = drop_every, drop_end = -1;
    int drop_turn = 0, drops = 0;
    std::vector<double> reacq;
    double busy_ms = 0;
    // gaps: per side (the tracker's left/right call, as the live probe counts them), the time from
    // the last processed set with a hand of that side to the next one, when it's 0.3 s or more; and
    // sets with two published palms within 8 cm (one hand counted twice)
    double side_last[2] = {-1, -1};
    std::vector<double> gaps[2];
    int overlap_sets = 0;
    // jitter: how far each update's palm is from a straight line through the last two,
    // mm (steady motion cancels out; what's left is noise and real acceleration)
    std::vector<double> jit_raw, jit_sm;
    int near_face = 0, hand_updates = 0;   // published palms within 20 cm of the eyes
    PinchParams middle_params = pinch_params;   // as ft-hands: the middle pinch, and the two exclude each other
    middle_params.finger = 12, middle_params.other_finger = 8, middle_params.other_margin = 0.01;
    middle_params.extended_finger = 8, middle_params.min_frames = 5;
    middle_params.dip = false;
    pinch_params.other_finger = 12;
    Pinch pinch(pinch_params);
    Pinch middle(middle_params);
    PinchParams fitted_params = pinch_params;
    fitted_params.fitted = true;
    Pinch pinch_fit(fitted_params);
    int pinch_fit_begins[2] = {0, 0};
    double pinch_begin_ts[2] = {0, 0};
    std::vector<double> pinch_len[2];   // seconds, per side
    int pinch_lost = 0;
    Grip grip(grip_params);
    // --publish-dump: the files ft-hands would write, in memory, appended per set
    FILE *pubf = publish_dump.empty() ? nullptr : std::fopen(publish_dump.c_str(), "wb");
    if (!publish_dump.empty() && !pubf) return std::fprintf(stderr, "%s: can't write it\n", publish_dump.c_str()), 1;
    static fh_hands_v2_t pub_hands;
    static fh_gestures_t pub_gestures;
    Publisher pub;
    GesturePublisher pub_g;
    if (pubf) {
        pub.use(&pub_hands);
        pub_g.use(&pub_gestures, pinch, grip);
        const uint32_t sizes[2] = {uint32_t(sizeof pub_hands), uint32_t(sizeof pub_gestures)};
        std::fwrite("FHDUMP02", 1, 8, pubf);
        std::fwrite(sizes, sizeof sizes, 1, pubf);
    }
    double grip_begin_ts[2] = {0, 0};
    std::vector<double> grip_len[2];
    do {
        if (sides == "file")
            for (auto &[first, rename] : file_runs)
                if (index + 1 >= first) names_swapped = rename;
        std::map<std::string, Image> images;
        // the set's time: the mono cameras' when they're used (the color ones run on another
        // clock); color frames can repeat across sets, so a set that doesn't move time on is skipped
        uint64_t t = UINT64_MAX, t_color = UINT64_MAX;
        for (size_t i = 0; i < cams.size(); ++i) {
            if (!used.count(cams[i].name)) continue;
            images[rename(cams[i].name)] = {px[i].data(), int(cams[i].width), int(cams[i].height), int(cams[i].width)};
            uint64_t &ti = std::string(cams[i].name).rfind("color_", 0) == 0 ? t_color : t;
            ti = std::min(ti, cams[i].capture_ns);
        }
        if (t == UINT64_MAX) t = t_color;
        if (t <= t_prev) {
            ++index;
            continue;
        }
        // a pause in the recording (the hand recorder's takes pause between prompts): gaps don't span it
        const bool pause = t_prev && t - t_prev > 500'000'000;
        t_prev = t;
        if (!t0) t0 = t;
        const double ts = (t - t0) / 1e9;
        ++index;
        if (ts < from) continue;
        if (ts > to) break;
        ++nsets;
        if (pause) {
            side_last[0] = side_last[1] = -1;
            if (tl) std::fprintf(tl, "%.3f break\n", ts);
        }

        if (t >= busy_until && t >= next_ns) {
            const auto w0 = std::chrono::steady_clock::now();
            const Stats before = tracker.stats;
            if (drop_every > 0 && drop_end < 0 && ts >= drop_next && last_out.size() == 2) {
                const Hand *h = last_out[0]->right() == bool(drop_turn) ? last_out[0] : last_out[1];
                if (tracker.blind(h->id, int64_t(t) + 350'000'000)) {
                    drop_end = ts + 0.35, ++drops, drop_turn ^= 1;
                    if (tl) std::fprintf(tl, "%.3f drop %d\n", ts, h->id);
                }
                drop_next = ts + drop_every;
            }
            const auto out = tracker.step(images, int64_t(t));
            if (drop_end >= 0 && ts >= drop_end && out.size() >= 2) {
                reacq.push_back(ts - drop_end);
                if (tl) std::fprintf(tl, "%.3f reacquired after %.2f s\n", ts, ts - drop_end);
                drop_end = -1, drop_next = std::max(drop_next, ts + 1.0);
            } else if (drop_end >= 0 && ts - drop_end > 10) {   // gone: give up, counts as 10 s
                reacq.push_back(10);
                drop_end = -1;
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
            if (cost) {   // repeatable: rounds of model calls at typical live costs, per thread
                const int hands = tracker.stats.hand_calls - before.hand_calls, palms = tracker.stats.palm_calls - before.palm_calls;
                ms = (2 + cost_hand * ((hands + threads - 1) / threads) + cost_palm * ((palms + threads - 1) / threads)) / slow;
            }
            busy_ms += ms;
            busy_until = t + uint64_t(ms * slow * 1e6) + 3'000'000;   // + the ring hand-off
            const std::vector<Seen> seen = tracker.views_now();
            if (tl)
                for (const std::string &e : trace) std::fprintf(tl, "%.3f ev %s\n", ts, e.c_str());
            trace.clear();
            if (first_hand_ts < 0 && !seen.empty()) first_hand_ts = ts;
            std::vector<Seen> side_views = seen;
            if (checking)
                for (Seen &v : side_check.probe(nets, pool, images, seen, int64_t(t))) side_views.push_back(v);
            if (checking && side_check.add(side_views, int64_t(t)) > 0 && side_check.verdict() != SideCheck::Undecided) {
                const bool backwards = side_check.verdict() == SideCheck::Swapped;
                char line[400];
                std::snprintf(line, sizeof line, "side cameras: %s %s at %.1f s (%.1f s after the first hand; %s)\n",
                              side_round ? "check" : "decision",
                              backwards ? (sides == "auto" ? "SWAPPED, exchanged" : "SWAPPED") : "as named", ts,
                              ts - first_hand_ts, side_check.summary().c_str());
                side_report += line;
                if (tl) std::fprintf(tl, "%.3f %s", ts, line);
                if (sides != "auto") {
                    checking = false;   // file, 0, 1: report only
                } else if (side_round == 0 || backwards) {
                    if (backwards) names_swapped = !names_swapped, tracker.exchange("slam_left", "slam_right");
                    side_check.reset();
                    side_check.min_clean = 20, side_check.min_votes = 40;
                    checking = ++side_round < 3;
                } else {
                    checking = false;
                }
            }
            std::vector<int> holding = pinch.holding(int64_t(t)), held2 = middle.holding(int64_t(t));   // as ft-hands
            holding.insert(holding.end(), held2.begin(), held2.end());
            grip.update(out, seen, int64_t(t), holding);
            pinch.update(out, seen, int64_t(t), grip.gripping(), middle.down());
            middle.update(out, seen, int64_t(t), grip.gripping(), pinch.down());
            if (tl)
                for (const Pinch::Event &e : middle.events)
                    std::fprintf(tl, "%.3f pinch2 %s %s d %.3f point %+.3f %+.3f %+.3f hand %u\n", ts, e.side ? "R" : "L",
                                 e.what, e.distance, e.point[0], e.point[1], e.point[2], middle.side(e.side).hand_id);
            if (pinch_fitted) {
                pinch_fit.update(out, seen, int64_t(t), grip.gripping());
                for (const Pinch::Event &e : pinch_fit.events) {
                    pinch_fit_begins[e.side] += std::string(e.what) == "begin";
                    if (tl) std::fprintf(tl, "%.3f pinch-fitted %s %s d %.3f\n", ts, e.side ? "R" : "L", e.what, e.distance);
                }
            }
            if (skeleton) {
                V3 up{0, 0, 0};
                if (!ups.empty()) {   // at the set's dequeue time, as frame-hands' armmodel.py
                    uint64_t dq = UINT64_MAX;
                    for (auto &c : cams)
                        if (used.count(c.name) && std::string(c.name).rfind("color_", 0) != 0) dq = std::min(dq, c.dqbuf_ns);
                    const auto it = std::lower_bound(ups.begin(), ups.end(), std::make_pair(int64_t(dq), V3{}),
                                                     [](auto &a, auto &b) { return a.first < b.first; });
                    if (it != ups.end() && std::llabs(it->first - int64_t(dq)) < 50'000'000) up = it->second;
                }
                for (const Hand *h : out) eval.add(*h, ts, up);
                eval.calibration(tracker.skeleton(), ts);
                label_hits.push_back({int64_t(t), int(out.size())});
            }
            if (rp)
                for (const Hand *h : out) {
                    const int sd = h->right() ? 1 : 0;
                    const bool pdown = (pinch.side(sd).flags & FH_PINCH_DOWN) && pinch.side(sd).hand_id == uint32_t(h->id);
                    const bool gdown = (grip.side(sd).flags & FH_PINCH_DOWN) && grip.side(sd).hand_id == uint32_t(h->id);
                    bool began = false;
                    for (const Pinch::Event &e : pinch.events) began = began || (e.side == sd && std::string(e.what) == "begin");
                    const bool fitted = h->fit.valid;
                    const V3 *pts = fitted ? h->fit.pts : h->smooth;
                    double q[4] = {0, 0, 0, 1};
                    if (fitted) skel::to_quat(h->fit.frames[0], q);
                    std::fprintf(rp, "%llu %d %s %d %d %d %d %d", (unsigned long long)t, h->id, sd ? "R" : "L",
                                 h->nviews >= 2, fitted, pdown, gdown, began);
                    for (int k = 0; k < 21; ++k)
                        for (int j = 0; j < 3; ++j) std::fprintf(rp, " %.5f", pts[k][j]);
                    std::fprintf(rp, " %.5f %.5f %.5f %.5f", q[0], q[1], q[2], q[3]);
                    for (int j = 0; j < 3; ++j) std::fprintf(rp, " %.4f", fitted ? h->fit.elbow[j] : 0.0);
                    std::fputc('\n', rp);
                }
            // as ft-hands (track/main.cpp), or a fixed rate (--rate)
            next_ns = t + uint64_t(((fixed_rate > 0 ? 1 / fixed_rate
                                                    : std::min({tracker.interval(),
                                                                pinch.engaged() || middle.engaged() || grip.engaged() ? 1 / 30.0 : 1.0,
                                                                want_hz > 0 ? 1 / want_hz : 1.0})) -
                                     0.005) * 1e9);
            if (tp) {
                std::fprintf(tp, "S %llu\n", (unsigned long long)t);
                for (const Pinch::Event &e : pinch.events)
                    std::fprintf(tp, "E %llu %d %s %.4f\n", (unsigned long long)t, e.side, e.what, e.distance);
                for (const Pinch::Event &e : middle.events)
                    std::fprintf(tp, "E2 %llu %d %s %.4f\n", (unsigned long long)t, e.side, e.what, e.distance);
                for (const Hand *h : out) {
                    // the pinch detector's measures (Pinch::update's measure) for this hand
                    auto measure = [&](int f, double m[3]) {
                        double sum = 0;
                        int n = 0;
                        for (const Seen &v : seen) {
                            if (v.hand != h->id) continue;
                            const V3 a{v.lm.world[4][0], v.lm.world[4][1], v.lm.world[4][2]};
                            const V3 b{v.lm.world[f][0], v.lm.world[f][1], v.lm.world[f][2]};
                            sum += norm(a - b), ++n;
                        }
                        m[0] = n ? sum / n * h->scale : -1;
                        m[1] = norm(h->pts[4] - h->pts[f]);
                        m[2] = h->fit.valid ? norm(h->fit.pts[4] - h->fit.pts[f]) : -1;
                    };
                    double m8[3], m12[3];
                    measure(8, m8), measure(12, m12);
                    const V3 *p = h->smooth;
                    V3 nrm = cross3(p[5] - p[0], p[17] - p[0]);
                    nrm = norm(nrm) > 0 ? nrm * ((h->right() ? 1.0 : -1.0) / norm(nrm)) : V3{0, 0, 0};
                    const V3 palm = (p[0] + p[5] + p[9] + p[13] + p[17]) * 0.2;
                    std::fprintf(tp, "H %llu %d %s %d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                                 (unsigned long long)t, h->id, h->right() ? "R" : "L", h->nviews, h->scale, m8[0], m8[1], m8[2],
                                 m12[0], m12[1], m12[2], nrm[0], nrm[1], nrm[2], palm[0], palm[1], palm[2]);
                    for (int k : {4, 8, 12})
                        for (const V3 *src : {h->pts, h->smooth, h->fit.valid ? h->fit.pts : nullptr})
                            for (int j = 0; j < 3; ++j) std::fprintf(tp, " %.4f", src ? src[k][j] : NAN);
                    for (const Seen &v : seen) {
                        if (v.hand != h->id) continue;
                        std::fprintf(tp, " %s %.3f", v.cam.c_str(), v.lm.presence);
                        for (int k : {4, 8, 12}) std::fprintf(tp, " %.1f %.1f", v.lm.pts[k][0], v.lm.pts[k][1]);
                    }
                    std::fputc('\n', tp);
                }
            }
            for (const Pinch::Event &e : grip.events) {
                if (std::string(e.what) == "begin") grip_begin_ts[e.side] = ts;
                else grip_len[e.side].push_back(ts - grip_begin_ts[e.side]);
                if (tl) std::fprintf(tl, "%.3f grip %s %s curl %.2f point %+.3f %+.3f %+.3f hand %u\n", ts, e.side ? "R" : "L",
                                     e.what, e.distance, e.point[0], e.point[1], e.point[2], grip.side(e.side).hand_id);
            }
            if (tl && (grip.curl[0] >= 0 || grip.curl[1] >= 0))
                std::fprintf(tl, "%.3f curl L %.2f R %.2f\n", ts, grip.curl[0], grip.curl[1]);
            for (const Pinch::Event &e : pinch.events) {
                if (std::string(e.what) == "begin") pinch_begin_ts[e.side] = ts;
                else pinch_len[e.side].push_back(ts - pinch_begin_ts[e.side]), pinch_lost += std::string(e.what) == "lost";
                if (tl) std::fprintf(tl, "%.3f pinch %s %s d %.3f point %+.3f %+.3f %+.3f hand %u\n", ts, e.side ? "R" : "L",
                                     e.what, e.distance, e.point[0], e.point[1], e.point[2], pinch.side(e.side).hand_id);
            }
            if (tl && (pinch.world_d[0] >= 0 || pinch.world_d[1] >= 0))   // both measures, for choosing one
                std::fprintf(tl, "%.3f pinchd L world %.3f tri %.3f R world %.3f tri %.3f palm %.2f %.2f\n", ts,
                             pinch.world_d[0], pinch.tri_d[0], pinch.world_d[1], pinch.tri_d[1], pinch.palm_down[0],
                             pinch.palm_down[1]);
            if (pubf) {
                pub.write(out, t);
                pub_g.write(pinch, grip, t);
                pub_hands.v1.publish_ns = pub_gestures.publish_ns = busy_until;
                std::fwrite(&pub_hands, sizeof pub_hands, 1, pubf);
                std::fwrite(&pub_gestures, sizeof pub_gestures, 1, pubf);
                int64_t dq = INT64_MAX;
                for (auto &c : cams)
                    if (used.count(c.name) && std::string(c.name).rfind("color_", 0) != 0) dq = std::min(dq, int64_t(c.dqbuf_ns));
                std::fwrite(&dq, sizeof dq, 1, pubf);
            }
            ++processed;
            last_out = out;
            bool l = false, r = false;
            for (const Hand *h : out) {
                (h->pts[0][0] < 0 ? l : r) = true;
                Track &tr = tracks[h->id];
                auto palm = [](const V3 *p) { return (p[0] + p[5] + p[9] + p[13] + p[17]) * 0.2; };
                const V3 raw = palm(h->pts), sm = palm(h->smooth);
                ++hand_updates, near_face += norm(sm) < 0.2;
                if (tr.line && ts - tr.t[0] >= 0.1) tr.line = 0;   // a gap: the line starts over
                if (tr.line >= 2 && tr.t[0] - tr.t[1] > 1e-3) {
                    const double k = (ts - tr.t[0]) / (tr.t[0] - tr.t[1]);
                    jit_raw.push_back(norm(raw - tr.raw[0] - (tr.raw[0] - tr.raw[1]) * k) * 1000);
                    jit_sm.push_back(norm(sm - tr.sm[0] - (tr.sm[0] - tr.sm[1]) * k) * 1000);
                }
                tr.raw[1] = tr.raw[0], tr.sm[1] = tr.sm[0], tr.t[1] = tr.t[0];
                tr.raw[0] = raw, tr.sm[0] = sm, tr.t[0] = ts;
                ++tr.line;
                if (!tr.sets) tr.first = ts;
                tr.last = ts, ++tr.sets, tr.left += h->pts[0][0] < 0;
                if (tl)
                    std::fprintf(tl, "%.3f %d %s %d %+.3f %+.3f %+.3f hand %s\n", ts, h->id, h->pts[0][0] < 0 ? "L" : "R", h->nviews,
                                 h->pts[0][0], h->pts[0][1], h->pts[0][2], h->right() ? "R" : "L");
                if (pp) {
                    double world[21][3] = {};
                    int n = 0;
                    for (const Seen &v : seen)
                        if (v.hand == h->id) {
                            for (int k = 0; k < 21; ++k)
                                for (int j = 0; j < 3; ++j) world[k][j] += v.lm.world[k][j];
                            ++n;
                        }
                    std::fprintf(pp, "%.4f %d %s %d %.3f", ts, h->id, h->right() ? "R" : "L", h->nviews, h->scale);
                    for (int k = 0; k < 21; ++k)
                        for (int j = 0; j < 3; ++j) std::fprintf(pp, " %.4f", n ? world[k][j] / n * h->scale : NAN);
                    for (int k = 0; k < 21; ++k)
                        for (int j = 0; j < 3; ++j) std::fprintf(pp, " %.4f", h->smooth[k][j]);
                    std::fputc('\n', pp);
                }
                if (dp) {
                    std::vector<const Seen *> vs;
                    for (const Seen &v : seen)
                        if (v.hand == h->id) vs.push_back(&v);
                    std::sort(vs.begin(), vs.end(), [](const Seen *a, const Seen *b) { return a->cam < b->cam; });
                    std::string names;
                    for (const Seen *v : vs) names += (names.empty() ? "" : "+") + v->cam;
                    std::fprintf(dp, "%.4f %d %s %d %s %.4f %.3f %.4f %.4f %.4f %.4f %.4f %.4f", ts, h->id,
                                 h->pts[0][0] < 0 ? "L" : "R", h->nviews, names.empty() ? "-" : names.c_str(), h->residual,
                                 h->scale, raw[0], raw[1], raw[2], sm[0], sm[1], sm[2]);
                    for (const Seen *v : vs) {
                        V3 mono[21];
                        const bool ok = tracker.single_view(used.at(v->cam), v->lm, h->scale, mono);
                        const V3 p = ok ? palm(mono) : V3{NAN, NAN, NAN};
                        std::fprintf(dp, " %s %.2f %.4f %.4f %.4f", v->cam.c_str(), v->lm.presence, p[0], p[1], p[2]);
                    }
                    std::fputc('\n', dp);
                }
            }
            if (tl && out.empty()) std::fprintf(tl, "%.3f -\n", ts);
            if (tl)
                for (const Seen &v : seen)
                    std::fprintf(tl, "%.3f   view %d %s presence %.2f roi %.0f %.0f %.0f %.3f  set %d\n", ts, v.hand, v.cam.c_str(),
                                 v.lm.presence, v.roi.center[0], v.roi.center[1], v.roi.size, v.roi.rotation, index);
            left += l, right += r, both += l && r;
            bool hl = false, hr = false;
            for (const Hand *h : out) (h->right() ? hr : hl) = true;
            for (int k = 0; k < 2; ++k)
                if (k ? hr : hl) {
                    if (side_last[k] >= 0 && ts - side_last[k] >= 0.3) gaps[k].push_back(ts - side_last[k]);
                    side_last[k] = ts;
                }
            {
                bool close = false;
                auto palm = [](const V3 *p) { return (p[0] + p[5] + p[9] + p[13] + p[17]) * 0.2; };
                for (size_t a = 0; a < out.size(); ++a)
                    for (size_t b = a + 1; b < out.size(); ++b) close = close || norm(palm(out[a]->pts) - palm(out[b]->pts)) < 0.08;
                overlap_sets += close;
            }
            ++hist[std::min<size_t>(out.size(), 2)];
        }

        if (oracle > 0 && nsets % oracle == 0) {
            const Stats keep = tracker.stats;
            const auto seen = tracker.exhaustive(images);
            tracker.stats = keep;
            bool l = false, r = false;
            for (const Seen &s : seen) {
                (s.wrist[0] < 0 ? l : r) = true;
                ++o_by_cam[s.cam + (s.wrist[0] < 0 ? " L" : " R")];
                if (tl)
                    std::fprintf(tl, "%.3f oracle %s tile %d presence %.2f right %.2f wrist %+.3f %+.3f %+.3f roi %.0f %.0f %.0f\n", ts,
                                 s.cam.c_str(), s.tile, s.lm.presence, s.lm.right, s.wrist[0], s.wrist[1], s.wrist[2],
                                 s.roi.center[0], s.roi.center[1], s.roi.size);
            }
            if (tl) std::fprintf(tl, "%.3f oracle-done %zu\n", ts, seen.size());
            {
                bool ol = false, orr = false;
                for (const Seen &s : seen) (s.lm.right > 0.5 ? orr : ol) = true;
                const int n = int(ol) + int(orr);
                o_hands += n, o_missed += std::max(0, n - int(std::min<size_t>(last_out.size(), 2)));
            }
            bool tl_ = false, tr_ = false;
            for (const Hand *h : last_out) (h->pts[0][0] < 0 ? tl_ : tr_) = true;
            ++o_sets;
            o_left += l, o_right += r;
            o_left_hit += l && tl_, o_right_hit += r && tr_;
            o_left_extra += !l && tl_, o_right_extra += !r && tr_;
            if (tl && ((!l && tl_) || (!r && tr_))) std::fprintf(tl, "%.3f oracle-extra %s%s set %d\n", ts, !l && tl_ ? "L" : "", !r && tr_ ? "R" : "", index);
        }
    } while (in.next(cams, px));
    if (tl) std::fclose(tl);
    if (dp) std::fclose(dp);
    if (pp) std::fclose(pp);
    if (pubf) std::fclose(pubf);
    if (rp) std::fclose(rp);
    if (tp) std::fclose(tp);

    const double secs = nsets > 1 ? nsets / 30.0 : 0;
    const Stats &s = tracker.stats;
    std::printf("%s: %d sets (%.0f s), processed %d (%.1f/s), %.1f ms per step\n", dir.c_str(), nsets, secs, processed,
                processed / std::max(secs, 1e-9), busy_ms / std::max(processed, 1));
    std::printf("hands per processed set: 0 %.0f%%, 1 %.0f%%, 2 %.0f%%;  a hand on the left %.0f%%, right %.0f%%, both %.0f%%\n",
                100.0 * hist[0] / processed, 100.0 * hist[1] / processed, 100.0 * hist[2] / processed,
                100.0 * left / processed, 100.0 * right / processed, 100.0 * both / processed);
    std::vector<double> lens[2];
    for (auto &[id, tr] : tracks) lens[tr.left * 2 > tr.sets ? 0 : 1].push_back(tr.last - tr.first);
    for (int k = 0; k < 2; ++k) {
        double total = 0;
        for (double d : lens[k]) total += d;
        std::printf("%s tracks: %zu, median %.1f s, total %.0f s\n", k ? "right" : "left ", lens[k].size(), median(lens[k]), total);
    }
    for (int k = 0; k < 2; ++k) {
        double total = 0, longest = 0;
        for (double g : gaps[k]) total += g, longest = std::max(longest, g);
        std::printf("%s hand gaps >= 0.3 s: %zu, total %.1f s, longest %.1f s\n", k ? "right" : "left ", gaps[k].size(), total, longest);
    }
    std::printf("sets with two hands within 8 cm: %d\n", overlap_sets);
    if (drop_every > 0) {
        std::sort(reacq.begin(), reacq.end());
        double sum = 0;
        for (double r : reacq) sum += r;
        auto q = [&](double f) { return reacq.empty() ? 0.0 : reacq[std::min(reacq.size() - 1, size_t(f * reacq.size()))]; };
        std::printf("re-acquire test: %d drops, %zu back; after the blind 0.35 s: median %.2f s, 75%% %.2f s, 90%% %.2f s, max %.2f s, "
                    "mean %.2f s\n", drops, reacq.size(), q(0.5), q(0.75), q(0.9), reacq.empty() ? 0 : reacq.back(),
                    reacq.empty() ? 0 : sum / reacq.size());
    }
    std::printf("views lost %d, handoff misses %d, dups %d, splits %d;  hands new %d, merged %d, forgotten %d\n", s.lost,
                s.handoff_miss, s.dups, s.splits, s.created, s.merged, s.forgotten);
    std::printf("model calls: palm %d (%.1f/s), hand %d (%.1f/s)\n", s.palm_calls, s.palm_calls / std::max(secs, 1e-9),
                s.hand_calls, s.hand_calls / std::max(secs, 1e-9));
    {
        std::vector<double> r, step;
        std::map<int, double> prev;
        for (auto &[id, x] : s.mono_ratio) {
            r.push_back(x);
            if (prev.count(id)) step.push_back(std::fabs(x - prev[id]));
            prev[id] = x;
        }
        std::sort(r.begin(), r.end());
        std::sort(step.begin(), step.end());
        if (!r.empty())
            std::printf("single-view distance / stereo: 10%% %.2f, median %.2f, 90%% %.2f; change between frames median %.3f, 90%% %.3f\n",
                        r[r.size() / 10], r[r.size() / 2], r[r.size() * 9 / 10], step[step.size() / 2], step[step.size() * 9 / 10]);
    }
    std::printf("palms within 20 cm of the eyes: %d of %d hand updates\n", near_face, hand_updates);
    for (int k = 0; k < 2; ++k) std::sort(pinch_len[k].begin(), pinch_len[k].end());
    std::printf("pinches (%s, %.3f/%.3f m, palm down under %.2f): left %zu (median %.2f s), right %zu (median %.2f s), "
                "%d ended by losing the hand, held back (palm down) left %d right %d\n",
                pinch_params.triangulated ? "triangulated tips" : "world landmarks", pinch_params.begin_m, pinch_params.end_m,
                pinch_params.palm_down_max,
                pinch_len[0].size(), pinch_len[0].empty() ? 0 : pinch_len[0][pinch_len[0].size() / 2], pinch_len[1].size(),
                pinch_len[1].empty() ? 0 : pinch_len[1][pinch_len[1].size() / 2], pinch_lost, pinch.held_back[0],
                pinch.held_back[1]);
    if (pinch_params.dip)
        std::printf("pinches begun on a dip (--pinch-dip): left %d, right %d\n", pinch.dip_begins[0], pinch.dip_begins[1]);
    for (int k = 0; k < 2; ++k) std::sort(grip_len[k].begin(), grip_len[k].end());
    std::printf("grips (curl under %.2f, open over %.2f): left %zu (median %.2f s), right %zu (median %.2f s)\n",
                grip_params.begin, grip_params.end, grip_len[0].size(),
                grip_len[0].empty() ? 0 : grip_len[0][grip_len[0].size() / 2], grip_len[1].size(),
                grip_len[1].empty() ? 0 : grip_len[1][grip_len[1].size() / 2]);
    std::sort(jit_raw.begin(), jit_raw.end());
    std::sort(jit_sm.begin(), jit_sm.end());
    if (!jit_raw.empty())
        std::printf("palm jitter (off a straight line through the last two updates): measured median %.1f mm, 90%% %.1f mm; "
                    "One Euro median %.1f mm, 90%% %.1f mm\n", jit_raw[jit_raw.size() / 2], jit_raw[jit_raw.size() * 9 / 10],
                    jit_sm[jit_sm.size() / 2], jit_sm[jit_sm.size() * 9 / 10]);
    if (!side_check.usable())
        std::printf("side cameras: not both in this recording\n");
    else
        std::printf("%sside cameras %s: %s\n", side_report.c_str(), side_report.empty() ? "undecided" : "at the end",
                    checking || side_report.empty() ? side_check.summary().c_str()
                                                    : names_swapped ? "exchanged from the recorded names" : "as recorded");
    if (pinch_fitted)
        std::printf("pinches on the fitted tips (same thresholds): left %d, right %d\n", pinch_fit_begins[0], pinch_fit_begins[1]);
    if (o_sets) {
        std::printf("oracle, %d sets: a left hand findable in %d, the tracker had it in %d (%.0f%%); right %d, had %d (%.0f%%)\n",
                    o_sets, o_left, o_left_hit, 100.0 * o_left_hit / std::max(o_left, 1), o_right, o_right_hit,
                    100.0 * o_right_hit / std::max(o_right, 1));
        std::printf("        tracker had a hand the full search didn't find: left %d, right %d\n", o_left_extra, o_right_extra);
        std::printf("        hands the full search found: %d, the tracker had %d fewer (%.1f%% missed)\n", o_hands, o_missed,
                    100.0 * o_missed / std::max(o_hands, 1));
        std::printf("        found by camera:");
        for (auto &[k, n] : o_by_cam) std::printf("  %s %d", k.c_str(), n);
        std::printf("\n");
    }
    if (skeleton) eval.report(tracker.skeleton(), int64_t(t0), label_hits);
    return 0;
}
