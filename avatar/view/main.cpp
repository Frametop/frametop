// ft-avatars: Frametop's avatar bodies, drawn. With a headset on it is the mirror that shows you
// the body you wear (live.h). With none it makes pictures of one (picture.h): a sheet to check
// an import by eye, the picture Settings shows for it, and what the mirror would show.
#include "../face/ownface.h"
#include "demo.h"
#include "live.h"
#include "picture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: ft-avatars --mirror MODEL [--height PX] [--seconds S] [--status S] [--no-voice] [--below] [--below-from DEGREES]\n"
                 "       ft-avatars --snapshot OUT.png MODEL [--pose NAME] [--at SECONDS] [--size PX] [--shape NAME=WEIGHT]...\n"
                 "       ft-avatars --mirror-snapshot OUT.png MODEL [the same]\n"
                 "       ft-avatars --below-snapshot OUT.png MODEL [the same]\n"
                 "       ft-avatars --thumb OUT.png MODEL [--size PX]\n"
                 "       ft-avatars --poses\n"
                 "       ft-avatars --hear SECONDS\n"
                 "poses: rest %s\n"
                 "shapes: aa ih ou ee oh blink blinkLeft blinkRight\n",
                 demo::kNames);
    return 2;
}

// "blink=1" into the weights. False for a shape that isn't one or a weight that isn't 0 to 1.
bool Shape(const char *arg, float weights[bodymesh::kShapes]) {
    static const char *const kNames[bodymesh::kShapes] = {"", "aa", "ih", "ou", "ee", "oh", "blink", "blinkLeft", "blinkRight"};
    const char *eq = std::strchr(arg, '=');
    if (!eq) return false;
    char *end = nullptr;
    const double w = std::strtod(eq + 1, &end);
    if (end == eq + 1 || *end || !(w >= 0 && w <= 1)) return false;
    for (int id = 1; id < bodymesh::kShapes; ++id)
        if (std::strlen(kNames[id]) == size_t(eq - arg) && !std::strncmp(arg, kNames[id], size_t(eq - arg))) return weights[id] = float(w), true;
    return false;
}

// --mirror MODEL and its options.
int Mirror(int argc, char **argv) {
    live::Ask ask;
    ask.model = argv[2];
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-voice")) {
            ask.voice = false;
            continue;
        }
        if (!std::strcmp(argv[i], "--below")) {
            ask.below = true;
            continue;
        }
        if (i + 1 >= argc) return Usage();
        if (!std::strcmp(argv[i], "--height")) ask.height = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds")) ask.seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--status")) ask.status = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--below-from")) ask.belowFrom = std::min(std::max(std::atof(argv[++i]), -90.0), 90.0) * 3.14159265358979323846 / 180;
        else return Usage();
    }
    return live::Mirror(ask);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--poses")) return std::printf("rest %s\n", demo::kNames), 0;
    if (argc >= 3 && !std::strcmp(argv[1], "--mirror")) return Mirror(argc, argv);
    if (argc == 3 && !std::strcmp(argv[1], "--hear")) return std::atof(argv[2]) > 0 ? ownface::Hear(std::atof(argv[2])) : Usage();
    if (argc < 4) return Usage();
    const bool thumb = !std::strcmp(argv[1], "--thumb"), glass = !std::strcmp(argv[1], "--mirror-snapshot"), below = !std::strcmp(argv[1], "--below-snapshot");
    if (!thumb && !glass && !below && std::strcmp(argv[1], "--snapshot")) return Usage();
    picture::Ask ask;
    ask.out = argv[2], ask.model = argv[3];
    if (thumb) ask.size = 256, ask.pose = "stand";
    for (int i = 4; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--size") && more) ask.size = std::atoi(argv[++i]);
        else if (!thumb && !std::strcmp(argv[i], "--pose") && more) ask.pose = argv[++i];
        else if (!thumb && !std::strcmp(argv[i], "--at") && more) ask.at = std::atof(argv[++i]);
        else if (!thumb && !std::strcmp(argv[i], "--shape") && more && Shape(argv[i + 1], ask.shapes)) ++i;
        else return Usage();
    }
    return thumb ? picture::Thumb(ask) : glass ? picture::Glass(ask) : below ? picture::Below(ask) : picture::Sheet(ask);
}
