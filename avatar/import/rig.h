// What finding a skeleton's bones by their names works on (handbones.cpp, bodybones.cpp): each
// node's name as words, how deep it hangs, and whether any vertex moves with it.
#pragma once

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "scene.h"

namespace importer {

// A name's words, lower case: split at anything that isn't a letter or digit, where a capital
// follows a small letter, and between letters and digits ("mixamorig:LeftHandIndex1" ->
// mixamorig left hand index 1).
std::vector<std::string> Words(const std::string &name);

inline bool HasWord(const std::vector<std::string> &words, const std::vector<std::string> &any) {
    for (const std::string &w : words)
        if (std::find(any.begin(), any.end(), w) != any.end()) return true;
    return false;
}

struct RigNodes {
    const Scene &s;
    std::vector<std::vector<std::string>> words;   // each node's name's
    std::vector<int> depth;
    std::vector<bool> skinned;                     // some vertex is moved by the node

    explicit RigNodes(const Scene &scene) : s(scene), words(scene.nodes.size()), depth(scene.nodes.size(), 0), skinned(scene.nodes.size(), false) {
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

    // The nodes `node` hangs from, nearest first, up to `stop` (left out) or the top.
    std::vector<int> Above(int node, int stop) const {
        std::vector<int> up;
        for (int p = s.nodes[size_t(node)].parent; p >= 0 && p != stop && up.size() <= s.nodes.size(); p = s.nodes[size_t(p)].parent) up.push_back(p);
        return up;
    }
};

}  // namespace importer
