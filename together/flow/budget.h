// The Frame's hardware decoder among the panels of other people shown here (docs/together.md,
// "Panels"). Its sessions and its load are one pool for the whole Frame: the wearer's own
// remote displays and PC windows draw on it, and so do the encoders of what they share. Other
// people's panels take the smaller part of it, kMostDecoders at once and kMostLoad macroblocks
// a second, and of that only what the hardware has left: once it gives a panel no decoder, no
// more are tried for kShortFor seconds, and then one more at a time.
//
// Who decodes goes by how much the wearer sees of each panel: a focused one before one in
// view, and among those the one focused last. A panel they don't see has no decoder. A panel
// without one is a still: its program keeps the last picture, and its owner hears that nobody
// sees it here (video.h), so nothing is sent for it.
//
// A panel takes the place of one that decodes and that the wearer sees less of, when no more
// may decode at once or the hardware gave it none: that one gives its decoder up first. If the
// hardware has none for it even in another's place, or there was no such panel, it is left a
// still for kAsideFor seconds, and the other has its decoder back.
//
// The rules only. Header only.
#pragma once

#include "../proto/messages.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace flow {

class Budget {
public:
    static constexpr int kMostDecoders = 6;
    static constexpr int kMostLoad = 2'592'000;   // macroblocks a second: a third of what the Frame decodes (docs/remote-displays.md)
    static constexpr double kShortFor = 10;
    static constexpr double kAsideFor = 30;

    // What is to be said to a panel's program: to decode, or to give its decoder up.
    struct Word {
        uint8_t panel;
        bool decode;
    };

    // What a panel's video costs the decoder, in macroblocks a second.
    static constexpr int Load(int width, int height, int pictures) { return ((width + 15) / 16) * ((height + 15) / 16) * pictures; }

    // A program shows `panel` here, and no longer.
    void Came(uint8_t panel, int load) {
        panels_[panel] = Panel{};
        panels_[panel].load = load;
    }
    void Went(uint8_t panel) { panels_.erase(panel); }

    // How much the wearer sees of the panel at `now` (proto::kHidden, kInView or kFocused).
    void Sees(uint8_t panel, int level, double now) {
        const auto it = panels_.find(panel);
        if (it == panels_.end()) return;
        Panel &p = it->second;
        if (level >= proto::kFocused || p.level >= proto::kFocused) p.focused = now;
        p.level = level;
    }
    // Its program says it has a decoder, or none any more.
    void Has(uint8_t panel, bool has) {
        const auto it = panels_.find(panel);
        if (it == panels_.end()) return;
        it->second.has = has;
        if (has) it->second.inPlace = false;
    }
    // The hardware gave its program none.
    void Refused(uint8_t panel, double now) {
        const auto it = panels_.find(panel);
        if (it == panels_.end()) return;
        Panel &p = it->second;
        p.told = p.has = false;
        bool below = false;   // one that decodes and that the wearer sees less of
        int busy = 0;
        for (const auto &[id, q] : panels_) {
            busy += q.told || q.has;
            below = below || (q.has && SeenMore(p, q));
        }
        if (!p.inPlace) limit_ = busy;   // what the hardware is known to give (in another's place, it gave that before)
        shortAt_ = now;
        if (p.inPlace || !below) p.aside = now + kAsideFor;   // (else Settle has the one below make room)
    }

    // What is to be said at `now`, after anything above and often besides.
    std::vector<Word> Settle(double now) {
        const int most = Most(now);
        std::vector<std::pair<uint8_t, Panel *>> order;
        for (auto &[id, p] : panels_) {
            p.want = false;
            if (p.level > proto::kHidden && now >= p.aside) order.push_back({id, &p});
        }
        std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return Before(a.first, *a.second, b.first, *b.second); });
        int n = 0, load = 0;
        for (auto &[id, p] : order)
            if (n < most && (n == 0 || load + p->load <= kMostLoad)) p->want = true, ++n, load += p->load;

        std::vector<Word> words;
        int busy = 0, busyLoad = 0;
        bool madeRoom = false;   // a panel the wearer sees gives its decoder up for another
        for (auto &[id, p] : panels_) {
            if (!p.want) p.inPlace = false;
            if (p.told && !p.want) {
                p.told = false, words.push_back({id, false});
                madeRoom = madeRoom || p.level > proto::kHidden;
            }
            if (p.told || p.has) ++busy, busyLoad += p.load;
        }
        for (auto &[id, p] : order)
            if (madeRoom && p->want && !p->told) p->inPlace = true;
        // One that is to decode is told once there is room: a decoder being given up still counts.
        for (auto &[id, p] : order) {
            if (!p->want || p->told) continue;
            if (!p->has) {
                if (busy >= most || (busy > 0 && busyLoad + p->load > kMostLoad)) continue;
                ++busy, busyLoad += p->load;
            }
            p->told = true, words.push_back({id, true});
        }
        return words;
    }

    // Whether the panel's program was told to decode.
    bool decodes(uint8_t panel) const {
        const auto it = panels_.find(panel);
        return it != panels_.end() && it->second.told;
    }

private:
    struct Panel {
        int load = 0, level = proto::kHidden;
        double focused = -1e9;   // when the wearer last had it focused
        double aside = -1e9;     // left a still until then
        bool told = false;       // its program was told to decode
        bool has = false;        // ...and says it has a decoder
        bool inPlace = false;    // it is to decode in the place of one that made room for it
        bool want = false;       // (Settle's)
    };

    // Whether the wearer sees more of `a` than of `b`: now, or when they last had each focused.
    static bool SeenMore(const Panel &a, const Panel &b) { return std::tie(a.level, a.focused) > std::tie(b.level, b.focused); }
    // ...and of two seen alike, the one with the lower id.
    static bool Before(uint8_t a, const Panel &pa, uint8_t b, const Panel &pb) { return std::tie(pb.level, pb.focused, a) < std::tie(pa.level, pa.focused, b); }
    int Most(double now) const { return int(std::min(double(kMostDecoders), limit_ + std::floor((now - shortAt_) / kShortFor))); }

    std::map<uint8_t, Panel> panels_;
    int limit_ = kMostDecoders;
    double shortAt_ = -1e9;
};

}  // namespace flow
