// Who gets a shared panel's next picture (docs/together.md, "The protocol"). A panel is encoded
// once for everyone who watches it, so a viewer is either in step with that video or waits for
// its next keyframe: a picture that leans on the one before is no use to someone who missed it.
//
// A viewer is out of step when they begin to watch, when they say their decoder lost its place,
// and when a picture sent to them was given up. A keyframe puts everyone back in step, and the
// encoder is asked for one at most every kKeyEvery seconds, whoever asks and however often: a
// peer can cost a panel two keyframes a second and no more.
//
// Each viewer also says how much of the panel they see, and the most any of them does is what
// the panel is encoded for (attention.h).
//
// The rules only, one object for each panel this Frame shares. Header only.
#pragma once

#include <algorithm>
#include <iterator>
#include <map>
#include <vector>

namespace flow {

class Viewers {
public:
    static constexpr double kKeyEvery = 0.5;

    // What the viewer on `link` says: how much of the panel they see (0 none of it, and more the
    // more they do), and whether they need a keyframe to go on.
    void Watch(int link, int level, bool fresh) {
        if (level <= 0) {
            viewers_.erase(link);
            return;
        }
        const auto [it, began] = viewers_.try_emplace(link);
        if (fresh && !began) it->second.inStep = false;
        it->second.level = level;
    }
    // The link is gone, or its person left the room.
    void Gone(int link) { viewers_.erase(link); }
    // Only the links `still(link)` holds for go on watching.
    template <class Still> void Keep(Still still) {
        for (auto it = viewers_.begin(); it != viewers_.end();) it = still(it->first) ? std::next(it) : viewers_.erase(it);
    }
    // A picture sent on `link` was given up: what follows it there has to be a keyframe.
    void GaveUp(int link) {
        const auto it = viewers_.find(link);
        if (it != viewers_.end()) it->second.inStep = false;
    }

    // Whether anyone watches: a panel nobody watches is not encoded at all.
    bool watched() const { return !viewers_.empty(); }
    size_t count() const { return viewers_.size(); }
    size_t waiting() const {
        size_t n = 0;
        for (const auto &[link, v] : viewers_) n += !v.inStep;
        return n;
    }
    // The most any of them sees of the panel, 0 with nobody watching.
    int level() const {
        int most = 0;
        for (const auto &[link, v] : viewers_) most = std::max(most, v.level);
        return most;
    }

    // Whether to ask the encoder for a keyframe at `now` (seconds). True once per ask.
    bool WantsKey(double now) {
        if (waiting() == 0 || (asked_ && now - askedAt_ < kKeyEvery)) return false;
        asked_ = true, askedAt_ = now;
        return true;
    }

    // The links the encoder's next picture goes to: a keyframe to all who watch, and they are in
    // step from then on; any other picture to those in step.
    std::vector<int> For(bool key) {
        std::vector<int> links;
        for (auto &[link, v] : viewers_) {
            if (key) v.inStep = true;
            if (v.inStep) links.push_back(link);
        }
        return links;
    }

private:
    struct Viewer {
        bool inStep = false;
        int level = 0;
    };
    std::map<int, Viewer> viewers_;   // by link, those who watch
    bool asked_ = false;
    double askedAt_ = 0;
};

}  // namespace flow
