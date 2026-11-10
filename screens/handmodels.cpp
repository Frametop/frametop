// See handmodels.h.
#include "handmodels.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <vector>

#include "../avatar/modelfile.h"

namespace handmodels {
namespace {

static_assert(handasset::kJoints == modelfile::kHandJoints, "the model file's joints");
static_assert(handasset::kPosStep == modelfile::kPosStep, "the model file's positions");
static_assert(kMaxName == modelfile::kMaxName, "the model file's names");

constexpr int kMaxOwn = 64;   // a person's models, at most: each holds memory while the program runs

// A person's model: the file's data, and the views of it the drawing code takes.
struct Own {
    std::string name;
    std::unique_ptr<modelfile::Model> data;
    handasset::Hand hands[2];
    unsigned generation = 0;
};

// A file as it was last tried, read or refused: it isn't read again until it changes.
struct Tried {
    std::string name;
    int64_t mtimeNs, size;
};

std::deque<Own> &Owns() {   // a deque: a model's hands stay where they are when another is added
    static std::deque<Own> v;
    return v;
}

std::vector<Tried> &Trieds() {
    static std::vector<Tried> v;
    return v;
}

handasset::Hand View(const modelfile::Hand &h) {
    handasset::Hand a = {h.verts(), h.tris(), h.pos.data(), h.nrm.data(), h.joints.data(), h.weights.data(), h.idx.data(), {}};
    std::memcpy(a.bind, h.bind, sizeof a.bind);
    return a;
}

Own *FindOwn(const std::string &name) {
    for (Own &o : Owns())
        if (o.name == name) return &o;
    return nullptr;
}

}  // namespace

int Count() { return handasset::kModels + int(Owns().size()); }

const char *Name(int model) {
    if (model >= handasset::kModels && model < Count()) return Owns()[size_t(model - handasset::kModels)].name.c_str();
    return handasset::kModel[model >= 0 && model < handasset::kModels ? model : 0].name;
}

const handasset::Hand &Hand(int model, bool right) {
    if (model >= handasset::kModels && model < Count()) return Owns()[size_t(model - handasset::kModels)].hands[right];
    return handasset::kModel[model >= 0 && model < handasset::kModels ? model : 0].hands[right];
}

unsigned Generation(int model) {
    return model >= handasset::kModels && model < Count() ? Owns()[size_t(model - handasset::kModels)].generation : 0;
}

int Find(const char *name) {
    for (int k = 0; k < handasset::kModels; ++k)
        if (!std::strcmp(handasset::kModel[k].name, name)) return k;
    for (size_t k = 0; k < Owns().size(); ++k)
        if (Owns()[k].name == name) return handasset::kModels + int(k);
    return -1;
}

bool NameOk(const std::string &name) {
    if (!modelfile::NameOk(name) || name == "robot" || name == "capsules") return false;
    for (const handasset::Model &m : handasset::kModel)
        if (name == m.name) return false;
    return true;
}

std::string Folder() { return modelfile::Folder(); }

int Scan(const std::string &folder, std::string &log) {
    DIR *dir = opendir(folder.c_str());
    if (!dir) return 0;   // no folder: no models of the person's own
    std::vector<std::string> names;
    const std::string ext = ".ftmodel";
    while (const dirent *e = readdir(dir)) {
        const std::string file = e->d_name;
        if (file.size() > ext.size() && !file.compare(file.size() - ext.size(), ext.size(), ext))
            names.push_back(file.substr(0, file.size() - ext.size()));
    }
    closedir(dir);
    std::sort(names.begin(), names.end());   // new models get their numbers in the names' order
    int changed = 0;
    for (const std::string &name : names) {
        const std::string path = folder + "/" + name + ext;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        const int64_t mtimeNs = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
        Tried *tried = nullptr;
        for (Tried &t : Trieds())
            if (t.name == name) tried = &t;
        if (tried && tried->mtimeNs == mtimeNs && tried->size == int64_t(st.st_size)) continue;
        if (!tried) {
            if (Trieds().size() >= size_t(4 * kMaxOwn)) continue;   // a folder full of files that aren't models
            Trieds().push_back({name, 0, 0});
            tried = &Trieds().back();
        }
        tried->mtimeNs = mtimeNs, tried->size = int64_t(st.st_size);
        Own *own = FindOwn(name);
        if (!own && !NameOk(name)) {
            log += path + ": not a name a model may have. ";
            continue;
        }
        if (!own && Owns().size() >= size_t(kMaxOwn)) {
            log += path + ": too many models. ";
            continue;
        }
        auto data = std::make_unique<modelfile::Model>();
        std::string err;
        if (!modelfile::ReadFile(path, *data, err)) {
            log += path + ": " + err + ". ";
            continue;
        }
        if (!data->hasHand[0] || !data->hasHand[1]) {
            log += path + ": it doesn't have both hands. ";
            continue;
        }
        if (!own) {
            Owns().emplace_back();
            own = &Owns().back();
            own->name = name;
        }
        own->data = std::move(data);
        for (int side = 0; side < 2; ++side) own->hands[side] = View(own->data->hands[side]);
        ++own->generation;
        ++changed;
    }
    return changed;
}

}  // namespace handmodels
