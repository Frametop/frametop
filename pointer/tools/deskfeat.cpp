// deskfeat: a recording's hands as the desk classifier's features (pointer/helper/deskclass.h), for
// hands/tools/desk_train.py.
//
//   deskfeat DUMP POSES.jsonl OUT.csv [--model MODEL.txt]
//
// DUMP: ft-handreplay TAKE --publish-dump DUMP (FHDUMP02: each set's dequeue time on CLOCK_MONOTONIC).
// POSES.jsonl: the same take's head poses (the hand recorder's poses.jsonl, CLOCK_MONOTONIC). Each set's
// hands go into the room with the head's pose at its dequeue time (a few ms after the exposure), as the
// pointer helper does with the capture time.
// OUT.csv: a header, then per set and seen hand: t (ns, CLOCK_MONOTONIC), side (0 left, 1 right), id,
// and the features. --model: then each class's probability by that model (p_free, p_keys, p_mouse), as
// the pointer helper would have them: to check a model in C++ against the trainer's numbers, or to see
// where it would park hands in a recording.
//
// Build: g++ -std=c++17 -O2 -I. pointer/tools/deskfeat.cpp -o build/deskfeat (no dependencies).
#include "pointer/helper/deskclass.h"
extern "C" {
#include "hands/include/fh_hands.h"
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using desk::V3;

struct Pose {
    double m[3][4];
    V3 Apply(const float p[3]) const {
        V3 r;
        r.x = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
        r.y = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
        r.z = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3];
        return r;
    }
    V3 Rotate(V3 v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
};

// poses.jsonl: the HMD's tracked samples (CLOCK_MONOTONIC ns, head -> room), in time order.
std::vector<std::pair<int64_t, Pose>> LoadPoses(const char *path) {
    std::vector<std::pair<int64_t, Pose>> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        long long t;
        const size_t hmd = line.find("\"hmd\": {");
        const size_t m = hmd == std::string::npos ? hmd : line.find("\"m\": [", hmd);
        const size_t ok = hmd == std::string::npos ? hmd : line.find("\"ok\": ", hmd);
        if (std::sscanf(line.c_str(), "{\"t\": %lld", &t) != 1 || m == std::string::npos || ok == std::string::npos ||
            line.compare(ok + 6, 4, "true") != 0)
            continue;
        Pose p;
        double v[12];
        if (std::sscanf(line.c_str() + m + 6, "%lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf, %lf", &v[0], &v[1],
                        &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) != 12)
            continue;
        for (int k = 0; k < 12; ++k) p.m[k / 4][k % 4] = v[k];
        out.push_back({t, p});
    }
    std::sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.first < b.first; });
    return out;
}

// The pose nearest t, if one is within 50 ms.
const Pose *PoseAt(const std::vector<std::pair<int64_t, Pose>> &poses, int64_t t) {
    auto it = std::lower_bound(poses.begin(), poses.end(), t, [](auto &a, int64_t v) { return a.first < v; });
    const std::pair<int64_t, Pose> *best = nullptr;
    if (it != poses.end()) best = &*it;
    if (it != poses.begin() && (!best || t - std::prev(it)->first < best->first - t)) best = &*std::prev(it);
    return best && std::llabs(best->first - t) < 50'000'000 ? &best->second : nullptr;
}

// Rotate v by quaternion q (x, y, z, w), as handsin.h.
V3 QuatRotate(const float q[4], V3 v) {
    const V3 u{q[0], q[1], q[2]};
    const double s = q[3];
    const V3 c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return u * (2 * desk::Dot(u, v)) + v * (s * s - desk::Dot(u, u)) + c * (2 * s);
}

}  // namespace

int main(int argc, char **argv) {
    desk::Model model;
    if (argc == 6 && !std::strcmp(argv[4], "--model")) {
        if (!model.Load(argv[5])) {
            std::fprintf(stderr, "%s: %s\n", argv[5], model.error.c_str());
            return 1;
        }
    } else if (argc != 4) {
        std::fprintf(stderr, "usage: %s DUMP POSES.jsonl OUT.csv [--model MODEL.txt]\n", argv[0]);
        return 2;
    }
    const auto poses = LoadPoses(argv[2]);
    if (poses.empty()) {
        std::fprintf(stderr, "%s: no head poses\n", argv[2]);
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    char magic[8];
    uint32_t sizes[2];
    if (!f.read(magic, 8) || std::memcmp(magic, "FHDUMP02", 8) || !f.read(reinterpret_cast<char *>(sizes), 8) ||
        sizes[0] < sizeof(fh_hands_t) || sizes[1] > 4096) {
        std::fprintf(stderr, "%s: not an FHDUMP02 publish dump\n", argv[1]);
        return 1;
    }
    FILE *out = std::fopen(argv[3], "w");
    if (!out) {
        std::perror(argv[3]);
        return 1;
    }
    std::fprintf(out, "t,side,id");
    for (int i = 0; i < desk::Features::kCount; ++i) std::fprintf(out, ",%s", desk::Features::Name(i));
    std::fprintf(out, model.Loaded() ? ",p_free,p_keys,p_mouse\n" : "\n");
    std::vector<char> hb(sizes[0]), gb(sizes[1]);
    static fh_hands_v2_t h;
    desk::Features feats;
    long sets = 0, rows = 0, headless = 0;
    while (f.read(hb.data(), sizes[0]) && f.read(gb.data(), sizes[1])) {
        int64_t dq;
        if (!f.read(reinterpret_cast<char *>(&dq), sizeof dq)) break;
        ++sets;
        std::memset(&h, 0, sizeof h);
        std::memcpy(&h, hb.data(), std::min(sizeof h, hb.size()));
        const Pose *head = PoseAt(poses, dq);
        if (!head) {
            ++headless;
            continue;
        }
        const bool v2 = h.v1.version >= 2 && sizes[0] >= sizeof h;
        desk::Hand hands[2];
        uint32_t ids[2] = {0, 0};
        for (uint32_t k = 0; k < std::min<uint32_t>(h.v1.nhands, FH_HANDS_MAX_HANDS); ++k) {
            const fh_hand_t &hh = h.v1.hands[k];
            const fh_hand_fit_t *fit = v2 && (h.fits[k].flags & FH_FIT_VALID) && h.fits[k].id == hh.id ? &h.fits[k] : nullptr;
            const int side = (hh.flags & FH_HAND_RIGHT) ? 1 : 0;
            desk::Hand &d = hands[side];
            if (d.seen) continue;  // two on one side: the first, as ft-pointer's typing hands
            d.seen = true, d.side = side, ids[side] = hh.id;
            d.stereo = hh.flags & FH_HAND_STEREO, d.fitted = fit != nullptr;
            for (int i = 0; i < 21; ++i) d.pts[i] = head->Apply(fit ? fit->joints[i].p : hh.pts[i]);
            if (fit) {
                d.normal = desk::Unit(head->Rotate(QuatRotate(fit->joints[0].q, V3{0, -1, 0})));
            } else {
                const V3 a = d.pts[5] - d.pts[0], b = d.pts[17] - d.pts[0];
                const V3 n{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
                d.normal = desk::Unit(side ? n : n * -1.0);
            }
        }
        desk::Head hd;
        hd.pos = {head->m[0][3], head->m[1][3], head->m[2][3]};
        hd.fwd = desk::Unit(head->Rotate(V3{0, 0, -1}));
        double feat[2][desk::Features::kCount];
        bool ok[2];
        feats.Frame(double(dq) / 1e9, hd, hands, feat, ok);
        for (int s = 0; s < 2; ++s) {
            if (!ok[s]) continue;
            std::fprintf(out, "%lld,%d,%u", (long long)dq, s, ids[s]);
            for (int i = 0; i < desk::Features::kCount; ++i) std::fprintf(out, ",%.5g", feat[s][i]);
            if (model.Loaded()) {
                double p[desk::Model::kClasses];
                model.Probs(feat[s], p);
                std::fprintf(out, ",%.4f,%.4f,%.4f", p[0], p[1], p[2]);
            }
            std::fprintf(out, "\n");
            ++rows;
        }
    }
    std::fclose(out);
    std::fprintf(stderr, "%ld sets, %ld hand rows, %ld sets without a head pose\n", sets, rows, headless);
    return rows ? 0 : 1;
}
