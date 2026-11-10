// ft-avatars: Frametop's avatar bodies, drawn. For now it makes pictures of one with no headset
// on (picture.h): a sheet to check an import by eye, and the picture Settings shows for it.
#include "demo.h"
#include "picture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage: ft-avatars --snapshot OUT.png MODEL [--pose NAME] [--at SECONDS] [--size PX] [--shape NAME=WEIGHT]...\n"
                 "       ft-avatars --thumb OUT.png MODEL [--size PX]\n"
                 "       ft-avatars --poses\n"
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

}  // namespace

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--poses")) return std::printf("rest %s\n", demo::kNames), 0;
    if (argc < 4) return Usage();
    const bool thumb = !std::strcmp(argv[1], "--thumb");
    if (!thumb && std::strcmp(argv[1], "--snapshot")) return Usage();
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
    return thumb ? picture::Thumb(ask) : picture::Sheet(ask);
}
