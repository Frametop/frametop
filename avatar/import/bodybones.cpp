// See bodybones.h.
#include "bodybones.h"

namespace importer {
namespace {

using namespace bodyfile;
using Names = std::vector<std::string>;

const Names kLeft = {"l", "left"}, kRight = {"r", "right"};
const Names kFingers = {"thumb", "index", "pointer", "forefinger", "middle", "mid", "ring", "little", "pinky", "pinkie", "finger"};

struct Named : RigNodes {
    using RigNodes::RigNodes;

    bool Sided(int node, bool right) const { return HasWord(words[size_t(node)], right ? kRight : kLeft) && !HasWord(words[size_t(node)], right ? kLeft : kRight); }
    bool Unsided(int node) const { return !HasWord(words[size_t(node)], kLeft) && !HasWord(words[size_t(node)], kRight); }

    // The shallowest node under `under` (-1: anywhere) that `is` one, -1 for none.
    template <typename F> int Find(int under, F &&is) const {
        int best = -1;
        for (size_t i = 0; i < s.nodes.size(); ++i)
            if ((under < 0 || Under(int(i), under)) && is(int(i)) && (best < 0 || depth[i] < depth[size_t(best)])) best = int(i);
        return best;
    }

    // The first of `chain` named with one of `any` (and none of `not`), else -1.
    int Pick(const std::vector<int> &chain, const Names &any, const Names &no = {}) const {
        for (const int n : chain)
            if (HasWord(words[size_t(n)], any) && !HasWord(words[size_t(n)], no)) return n;
        return -1;
    }

    // `chain` without the nodes that sit where the one before them does (a rig's helper bones).
    std::vector<int> Apart(int from, const std::vector<int> &chain) const {
        std::vector<int> out;
        Vec last = s.nodes[size_t(from)].at;
        for (const int n : chain)
            if ((s.nodes[size_t(n)].at - last).norm() > 1e-4) out.push_back(n), last = s.nodes[size_t(n)].at;
        return out;
    }

    void Trunk(BodyBones &b) const {
        const int hips = b.node[kHips] = Find(-1, [&](int n) { return HasWord(words[size_t(n)], {"hips", "hip", "pelvis"}) && Unsided(n); });
        if (hips < 0) return;
        const int head = b.node[kHead] = Find(hips, [&](int n) { return HasWord(words[size_t(n)], {"head"}) && !HasWord(words[size_t(n)], {"top", "end", "nub", "tip"}) && Unsided(n); });
        if (head < 0) return;
        std::vector<int> spine = Above(head, hips);   // the head's side first
        std::reverse(spine.begin(), spine.end());
        const int neck = b.node[kNeck] = Pick(spine, {"neck"});
        if (neck >= 0) spine.erase(std::find(spine.begin(), spine.end(), neck), spine.end());
        if (!spine.empty()) b.node[kSpine] = spine[0];
        if (spine.size() >= 2) b.node[kChest] = spine[1];
        if (spine.size() >= 3) b.node[kUpperChest] = spine.back();
        b.node[kJaw] = Find(head, [&](int n) { return HasWord(words[size_t(n)], {"jaw"}); });
        for (int right = 0; right < 2; ++right)
            b.node[right ? kRightEye : kLeftEye] = Find(head, [&](int n) {
                return HasWord(words[size_t(n)], {"eye"}) && Sided(n, right) && !HasWord(words[size_t(n)], {"brow", "lid", "lash", "highlight", "end"});
            });
    }

    void Arm(BodyBones &b, bool right, const HandBones *found) const {
        const int hips = b.node[kHips], base = right ? kRightShoulder : kLeftShoulder;
        int hand = found ? found->joint[0] : -1;
        if (hand < 0) hand = Find(hips, [&](int n) { return HasWord(words[size_t(n)], {"hand", "wrist"}) && Sided(n, right) && !HasWord(words[size_t(n)], kFingers); });
        if (hand < 0 || !Under(hand, hips)) return;
        b.node[base + 3] = hand;
        // the bones above the hand, up to the spine's
        std::vector<int> trunk = Above(b.node[kHead], hips);
        trunk.push_back(hips);
        std::vector<int> arm;
        for (const int n : Above(hand, -1)) {
            if (std::find(trunk.begin(), trunk.end(), n) != trunk.end()) break;
            arm.push_back(n);
        }
        arm = Apart(hand, arm);
        if (arm.size() < 2) return;
        int lower = Pick(arm, {"forearm", "fore", "lower", "elbow"}), shoulder = Pick(arm, {"shoulder", "clavicle", "collar"});
        if (lower < 0) lower = arm[0];
        int upper = -1;
        for (const int n : arm)   // the first above the forearm that isn't the shoulder
            if (upper < 0 && depth[size_t(n)] < depth[size_t(lower)] && n != shoulder) upper = n;
        if (upper < 0) return;
        if (shoulder < 0 && arm.back() != upper) shoulder = arm.back();
        b.node[base + 2] = lower, b.node[base + 1] = upper;
        if (shoulder >= 0 && depth[size_t(shoulder)] < depth[size_t(upper)]) b.node[base] = shoulder;
    }

    void Leg(BodyBones &b, bool right) const {
        const int hips = b.node[kHips], base = right ? kRightUpperLeg : kLeftUpperLeg;
        const int foot = Find(hips, [&](int n) { return HasWord(words[size_t(n)], {"foot", "ankle"}) && Sided(n, right) && !HasWord(words[size_t(n)], {"toe", "toes", "ball"}); });
        if (foot < 0) return;
        b.node[base + 2] = foot;
        const std::vector<int> leg = Apart(foot, Above(foot, hips));
        if (leg.size() < 2) return;
        b.node[base + 1] = leg[0], b.node[base] = leg.back();
        b.node[base + 3] = Find(foot, [&](int n) { return HasWord(words[size_t(n)], {"toe", "toes", "ball"}) && !HasWord(words[size_t(n)], {"end", "tip", "nub"}); });
    }
};

// A hand's finger bones as the body's: WebXR's joints 1 to 3 for the thumb, and each finger's
// three after its metacarpal.
void Fingers(const HandBones &hand, bool right, BodyBones &b) {
    const int base = right ? kRightFingers : kLeftFingers;
    for (int f = 0; f < 5; ++f)
        for (int k = 0; k < 3; ++k) b.node[base + 3 * f + k] = hand.joint[(f == 0 ? 1 : 5 * f + 1) + k];
}

}  // namespace

bool FindBody(const Scene &s, const HandBones *const hands[2], BodyBones &out, std::string &why) {
    std::fill(out.node, out.node + kRoles, -1);
    if (!s.humanoid.empty()) {
        for (int r = 0; r < kRoles; ++r) {
            const auto it = s.humanoid.find(kRoleNames[r]);
            if (it != s.humanoid.end() && it->second >= 0 && it->second < int(s.nodes.size())) out.node[r] = it->second;
        }
    } else {
        const Named rig(s);
        rig.Trunk(out);
        if (out.node[kHips] >= 0 && out.node[kHead] >= 0)
            for (int right = 0; right < 2; ++right) rig.Arm(out, right, hands[right]), rig.Leg(out, right);
    }
    for (int right = 0; right < 2; ++right)   // the fingers the hand was found with, where the humanoid didn't name them all
        if (hands[right] && hands[right]->joint[0] == out.node[right ? kRightHand : kLeftHand]) Fingers(*hands[right], right, out);
    std::string missing;
    for (int r = 0; r < kRoles; ++r)
        if (RoleNeeded(r) && out.node[r] < 0) missing += (missing.empty() ? "" : ", ") + std::string(kRoleNames[r]);
    if (!missing.empty()) return why = "its skeleton has no bone that can be told as: " + missing, false;
    for (int r = 0; r < kRoles; ++r)
        for (int o = 0; o < r; ++o)
            if (out.node[r] >= 0 && out.node[r] == out.node[o]) return why = std::string("one bone is both ") + kRoleNames[o] + " and " + kRoleNames[r], false;
    return true;
}

}  // namespace importer
