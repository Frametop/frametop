// See handbones.h.
#include "handbones.h"

#include <algorithm>
#include <cctype>

namespace importer {
namespace {

const char *const kFingerName[5] = {"thumb", "index finger", "middle finger", "ring finger", "little finger"};
// The words a rig names each finger with.
const std::vector<std::string> kFingerWords[5] = {
    {"thumb"}, {"index", "pointer", "forefinger"}, {"middle", "mid"}, {"ring"}, {"little", "pinky", "pinkie"}};
// VRM's names for a finger's bones, after "left" or "right": the thumb's, then the others'.
const char *const kVrmFinger[5] = {"Thumb", "Index", "Middle", "Ring", "Little"};
const char *const kVrmThumbPart[3] = {"Metacarpal", "Proximal", "Distal"};
const char *const kVrmPart[3] = {"Proximal", "Intermediate", "Distal"};

const std::vector<std::string> kHandWords = {"hand", "wrist"}, kLeftWords = {"l", "left"}, kRightWords = {"r", "right"};

bool Has(const std::vector<std::string> &words, const std::vector<std::string> &any) {
    for (const std::string &w : words)
        if (std::find(any.begin(), any.end(), w) != any.end()) return true;
    return false;
}

// The finger a name is for (0 thumb .. 4 little), -1 for none, -2 for more than one.
int FingerOf(const std::vector<std::string> &words) {
    int found = -1;
    for (int f = 0; f < 5; ++f)
        if (Has(words, kFingerWords[f])) found = found == -1 ? f : -2;
    return found;
}

struct Rig {
    const Scene &s;
    std::vector<std::vector<std::string>> words;   // each node's name's
    std::vector<int> depth;
    std::vector<bool> skinned;                     // some vertex is moved by the node

    explicit Rig(const Scene &scene) : s(scene), words(scene.nodes.size()), depth(scene.nodes.size(), 0), skinned(scene.nodes.size(), false) {
        for (size_t i = 0; i < s.nodes.size(); ++i) {
            words[i] = Words(s.nodes[i].name);
            int d = 0;
            for (int p = s.nodes[i].parent; p >= 0 && d < int(s.nodes.size()); p = s.nodes[size_t(p)].parent) ++d;
            depth[i] = d;
        }
        for (const Vertex &v : s.verts)
            for (int k = 0; k < kInfluences; ++k)
                if (v.node[k] >= 0 && v.weight[k] > 0) skinned[size_t(v.node[k])] = true;
    }

    bool Under(int node, int ancestor) const {
        int steps = 0;
        for (int p = s.nodes[size_t(node)].parent; p >= 0 && steps <= int(s.nodes.size()); p = s.nodes[size_t(p)].parent, ++steps)
            if (p == ancestor) return true;
        return false;
    }

    // Whether the nodes, sorted by depth, are one line of descent.
    bool OneLine(const std::vector<int> &chain) const {
        for (size_t k = 1; k < chain.size(); ++k)
            if (!Under(chain[k], chain[k - 1])) return false;
        return true;
    }

    // Whether a node is in the hand's reach: under it, or for a flat rig (every joint a child of
    // one node, as WebXR's hands are made) beside it and not named for the other side.
    bool Within(int node, int hand, bool flat) const {
        if (!flat) return Under(node, hand);
        const std::vector<std::string> &other = Has(words[size_t(hand)], kLeftWords) ? kRightWords : kLeftWords;
        const bool sided = Has(words[size_t(hand)], kLeftWords) || Has(words[size_t(hand)], kRightWords);
        return node != hand && s.nodes[size_t(node)].parent == s.nodes[size_t(hand)].parent && !(sided && Has(words[size_t(node)], other));
    }

    // The hand's nodes named for the finger, nearest the wrist first. Empty with `why`.
    std::vector<int> Chain(int hand, int finger, bool flat, std::string &why) const {
        std::vector<int> chain;
        for (size_t i = 0; i < s.nodes.size(); ++i)
            if (FingerOf(words[i]) == finger && Within(int(i), hand, flat)) chain.push_back(int(i));
        if (chain.empty()) return why = std::string("no bones named for the ") + kFingerName[finger], chain;
        if (flat) {   // no line of descent to go by: along the finger, by how far each is from the wrist
            const Vec w = s.nodes[size_t(hand)].at;
            std::stable_sort(chain.begin(), chain.end(), [&](int a, int b) { return (s.nodes[size_t(a)].at - w).norm() < (s.nodes[size_t(b)].at - w).norm(); });
            return chain;
        }
        const auto byDepth = [&](int a, int b) { return depth[size_t(a)] < depth[size_t(b)]; };
        std::stable_sort(chain.begin(), chain.end(), byDepth);
        if (!OneLine(chain)) {
            // helper bones beside the real ones (a control rig's): the ones the skin uses decide
            std::vector<int> used;
            for (const int n : chain)
                if (skinned[size_t(n)]) used.push_back(n);
            chain = used;
            if (chain.empty() || !OneLine(chain)) return why = std::string("the ") + kFingerName[finger] + "'s bones branch", std::vector<int>();
        }
        return chain;
    }

    // A finger's joints from its chain, by how many nodes it has (handbones.h).
    bool FromChain(int hand, int finger, const std::vector<int> &chain, HandBones &out, std::string &why) const {
        const size_t n = chain.size();
        int *joint = out.joint + (finger == 0 ? 1 : 5 * finger);   // the thumb's four, a finger's five
        if (finger == 0) {
            if (n != 3 && n != 4) return why = "the thumb has " + std::to_string(n) + " bones: 3 or 4 are read", false;
            for (size_t k = 0; k < n; ++k) joint[k] = chain[k];
            return true;
        }
        size_t first = 1;   // the chain's first node is the knuckle
        if (n == 5) first = 0;
        else if (n == 4) {
            // a metacarpal starts near the wrist; a knuckle is about half way to the chain's end
            const Vec w = s.nodes[size_t(hand)].at;
            const double near = (s.nodes[size_t(chain[0])].at - w).norm(), far = (s.nodes[size_t(chain[3])].at - w).norm();
            if (far > 1e-9 && near / far < 0.38) first = 0;
        } else if (n != 3) {
            return why = std::string("the ") + kFingerName[finger] + " has " + std::to_string(n) + " bones: 3 to 5 are read", false;
        }
        for (size_t k = 0; k < n; ++k) joint[first + k] = chain[k];
        return true;
    }

    // Every finger under a hand node. `vrmSide`: "left" or "right" when the file's humanoid names that hand's bones.
    bool Fingers(int hand, const char *vrmSide, HandBones &out, std::string &why) const {
        std::fill(out.joint, out.joint + kHandJoints, -1);
        out.joint[0] = hand;
        bool flat = !vrmSide;
        for (size_t i = 0; i < s.nodes.size() && flat; ++i)
            if (FingerOf(words[i]) >= 0 && Under(int(i), hand)) flat = false;
        for (int f = 0; f < 5; ++f) {
            std::vector<int> chain;
            for (int k = 0; vrmSide && k < 3; ++k) {
                const auto it = s.humanoid.find(std::string(vrmSide) + kVrmFinger[f] + (f == 0 ? kVrmThumbPart[k] : kVrmPart[k]));
                if (it != s.humanoid.end()) chain.push_back(it->second);
            }
            if (chain.size() == 3) {
                if (!Under(chain[0], hand) || !OneLine(chain)) return why = std::string("the ") + kFingerName[f] + "'s bones aren't under the hand", false;
            } else {
                chain = Chain(hand, f, flat, why);   // a VRM may leave fingers out of its humanoid: their names then
                if (chain.empty()) return false;
            }
            if (!FromChain(hand, f, chain, out, why)) return false;
        }
        return true;
    }
};

}  // namespace

std::vector<std::string> Words(const std::string &name) {
    std::vector<std::string> out;
    std::string word;
    const auto end = [&] {
        if (!word.empty()) out.push_back(word);
        word.clear();
    };
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c) || c >= 0x80) {
            end();
            continue;
        }
        if (!word.empty()) {
            const unsigned char p = static_cast<unsigned char>(name[i - 1]);
            const unsigned char next = i + 1 < name.size() ? static_cast<unsigned char>(name[i + 1]) : 0;
            const bool smallToCapital = std::islower(p) && std::isupper(c);
            const bool capitalsThenWord = std::isupper(p) && std::isupper(c) && std::islower(next);   // "LThumb": L, Thumb
            if (smallToCapital || capitalsThenWord || bool(std::isdigit(p)) != bool(std::isdigit(c))) end();
        }
        word += char(std::tolower(c));
    }
    end();
    return out;
}

bool FindHand(const Scene &s, Side side, HandBones &out, std::string &why) {
    const Rig rig(s);
    const char *name = side == Side::Right ? "right" : "left";
    if (side != Side::Unsaid) {
        const auto it = s.humanoid.find(std::string(name) + "Hand");
        if (it != s.humanoid.end()) return rig.Fingers(it->second, name, out, why);
    }
    // a hand or wrist bone the skin uses (a mesh's own node may be named for the hand too)
    std::vector<int> hands;
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        const std::vector<std::string> &w = rig.words[i];
        if (!rig.skinned[i] || !Has(w, kHandWords) || FingerOf(w) != -1) continue;
        if (side != Side::Unsaid && !Has(w, side == Side::Left ? kLeftWords : kRightWords)) continue;
        if (side == Side::Unsaid && (Has(w, kLeftWords) || Has(w, kRightWords))) continue;
        hands.push_back(int(i));
    }
    // the deepest that has every finger under it: a wrist bone with a hand bone below is passed over for the hand
    std::stable_sort(hands.begin(), hands.end(), [&](int a, int b) { return rig.depth[size_t(a)] > rig.depth[size_t(b)]; });
    std::string first;
    for (const int hand : hands) {
        std::string w;
        if (rig.Fingers(hand, nullptr, out, w)) return true;
        if (first.empty()) first = w + " (under " + s.nodes[size_t(hand)].name + ")";
    }
    why = hands.empty() ? std::string("no bone named for the ") + (side == Side::Unsaid ? "" : std::string(name) + " ") + "hand" : first;
    return false;
}

}  // namespace importer
