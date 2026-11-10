// ft-model-import: a model from someone else's format into Frametop's own file (modelfile.h),
// in your avatars folder, where ft-handview finds it (HANDS_VIEW_MODEL; Frametop Settings'
// Appearance page runs this).
//
//   ft-model-import FILE [--name NAME] [--to DIR] [--side left|right] [--may-share] [--check]
//
//   FILE         a VRM (0.x or 1.0), a glTF or GLB, or an FBX: an avatar, or a hand or a pair
//   --name NAME  the model's name (its file is NAME.ftmodel): lower-case letters, digits, "-"
//                and "_". Default: made from FILE's name. ft-handview doesn't read a file named
//                as one of its own models (asset, robot, ...: screens/handmodels.h)
//   --to DIR     where to write (default: $FT_AVATARS_DIR, or ~/.local/share/frametop/avatars)
//   --side       which hand a file of one hand is, when its bones' names and its own don't say
//   --may-share  you may pass this model on to the people you share a room with (a glTF or an
//                FBX can't say; a VRM's licence does, and this can't override it)
//   --check      read and convert, write nothing
//
// For now it takes the model's hands (docs/together.md: the body is next). A model with one
// hand gets the other mirrored. Nothing is guessed: a skeleton whose hand bones can't be told
// (handbones.h) is refused, with what is missing.
//
// Prints what it did, a fact a line ("key value": Settings reads them), and exits 0; or
// "error WHY" and exits 1. "share" is yes, no (a glTF or FBX without --may-share) or licence
// (the VRM's licence forbids it).
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "handpart.h"

namespace {

using namespace importer;

int Fail(const std::string &why) {
    std::printf("error %s\n", why.c_str());
    return 1;
}

int Usage() {
    std::fprintf(stderr, "usage: ft-model-import FILE [--name NAME] [--to DIR] [--side left|right] [--may-share] [--check]\n");
    return 2;
}

// A path's file name without its folders, split at its last dot: "a/My Hand.glb" is "My Hand" and "glb".
void NameParts(const std::string &path, std::string &stem, std::string &ext) {
    const size_t slash = path.rfind('/');
    const std::string file = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = file.rfind('.');
    stem = file.substr(0, dot), ext = dot == std::string::npos ? "" : file.substr(dot + 1);
    for (char &c : ext)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
}

// Why no hand was found on either side: what was wrong with the bones that were, or that none were.
std::string NoHands(const std::string why[2], const std::string &unsaid) {
    std::string out;
    for (const std::string *w : {&why[0], &why[1], &unsaid})
        if (w->rfind("no bone named for the", 0) != 0 && out.find(*w) == std::string::npos) out += (out.empty() ? "" : "; ") + *w;
    return out.empty() ? "no bone the mesh moves with is named for a hand (hand or wrist, with left or right, or l or r)" : out;
}

// The folder and every folder above it that isn't there yet.
bool MakeFolder(const std::string &dir) {
    for (size_t at = 1; at <= dir.size(); ++at)
        if (at == dir.size() || dir[at] == '/') {
            const std::string part = dir.substr(0, at);
            if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    std::string file, name, to;
    Side said = Side::Unsaid;
    bool mayShare = false, check = false;
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--name") && more) name = argv[++i];
        else if (!std::strcmp(argv[i], "--to") && more) to = argv[++i];
        else if (!std::strcmp(argv[i], "--side") && more && (!std::strcmp(argv[i + 1], "left") || !std::strcmp(argv[i + 1], "right")))
            said = argv[++i][0] == 'l' ? Side::Left : Side::Right;
        else if (!std::strcmp(argv[i], "--may-share")) mayShare = true;
        else if (!std::strcmp(argv[i], "--check")) check = true;
        else if (argv[i][0] != '-' && file.empty()) file = argv[i];
        else return Usage();
    }
    if (file.empty()) return Usage();
    std::string stem, ext;
    NameParts(file, stem, ext);
    if (name.empty()) name = modelfile::NameFrom(stem);
    if (!modelfile::NameOk(name)) return Fail("the name may have lower-case letters, digits, - and _ only, 31 at most");

    Scene scene;
    std::string err;
    if (ext == "vrm" || ext == "glb" || ext == "gltf") {
        if (!LoadGltf(file, scene, err)) return Fail(err);
    } else if (ext == "fbx") {
        if (!LoadFbx(file, scene, err)) return Fail(err);
    } else {
        return Fail("not a VRM, glTF, GLB or FBX file by its name");
    }

    modelfile::Model model;
    HandReport report[2];
    std::string why[2];
    for (int side = 0; side < 2; ++side) {
        HandBones bones;
        model.hasHand[side] = FindHand(scene, side ? Side::Right : Side::Left, bones, why[side]) &&
                              CutHand(scene, bones, model.hands[side], report[side], why[side]);
    }
    if (!model.hasHand[0] && !model.hasHand[1]) {
        // a file of one hand whose bones don't say which: the option does, or the file's name
        HandBones bones;
        std::string unsaid;
        if (!FindHand(scene, Side::Unsaid, bones, unsaid)) return Fail("no hands were found: " + NoHands(why, unsaid));
        Side side = said;
        for (const std::string &w : Words(stem)) {
            if (side != Side::Unsaid) break;
            if (w == "l" || w == "left") side = Side::Left;
            else if (w == "r" || w == "right") side = Side::Right;
        }
        if (side == Side::Unsaid) return Fail("the file doesn't say which hand it is: give --side left or --side right");
        const int k = side == Side::Right;
        if (!CutHand(scene, bones, model.hands[k], report[k], unsaid)) return Fail(unsaid);
        model.hasHand[k] = true;
    }
    bool mirrored[2] = {false, false};
    for (int side = 0; side < 2; ++side)
        if (!model.hasHand[side]) {
            model.hands[side] = Mirrored(model.hands[1 - side]);
            model.hasHand[side] = mirrored[side] = true;
            report[side] = report[1 - side];
        }

    model.info = scene.info;
    if (model.info.name.empty()) model.info.name = stem;
    // a VRM's licence said whether it may be passed on; for the rest, the person does
    const bool vrm = scene.info.format.rfind("vrm", 0) == 0;
    if (!vrm) model.info.mayShare = mayShare;
    const char *share = model.info.mayShare ? "yes" : scene.licenceSaysNo ? "licence" : "no";

    if (to.empty()) to = modelfile::Folder();
    const std::string path = to + "/" + name + ".ftmodel";
    if (!check) {
        if (!MakeFolder(to)) return Fail(to + ": " + std::strerror(errno));
        if (!modelfile::WriteFile(path, model, err)) return Fail(err);
    }
    std::printf("ok %s\n", name.c_str());
    if (!check) std::printf("file %s\n", path.c_str());
    std::printf("name %s\nformat %s\n", modelfile::Plain(model.info.name).c_str(), model.info.format.c_str());
    if (!model.info.author.empty()) std::printf("author %s\n", model.info.author.c_str());
    if (!model.info.licence.empty()) std::printf("licence %s\n", model.info.licence.c_str());
    std::printf("share %s\n", share);
    for (int side = 0; side < 2; ++side) {
        std::printf("%s %d triangles", side ? "right" : "left", report[side].tris);
        if (report[side].tris != report[side].sourceTris) std::printf(", reduced from %d", report[side].sourceTris);
        if (report[side].scale != 1) std::printf(", scaled %.3g times", report[side].scale);
        if (mirrored[side]) std::printf(", mirrored from the %s", side ? "left" : "right");
        std::printf("\n");
    }
    return 0;
}
