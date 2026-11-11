// The sizes the Frame's encoder is given (together/media/irisenc.h): a width it would take the
// Frame down with is refused before the device is touched. The device here is /dev/null, so
// the test says the same on a machine with the encoder and on one without.
#include "../media/irisenc.h"

#include <cstdio>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// How an Open at that size ends: 'r' refused for its size, 'd' the device was asked.
static char opened(int width, int height) {
    irisenc::Encoder enc;
    irisenc::Config cfg;
    cfg.width = width, cfg.height = height, cfg.dmabuf = true, cfg.device = "/dev/null";
    std::string err;
    if (enc.Open(cfg, err) || enc.isOpen()) return '?';
    if (err.find("isn't given a picture of " + std::to_string(width) + "x" + std::to_string(height)) != std::string::npos) return 'r';
    return err.find("S_FMT") != std::string::npos ? 'd' : '?';
}

int main() {
    using irisenc::Takes;
    check("the sizes seen to work are taken", Takes(640, 360) && Takes(1280, 800) && Takes(1920, 1080) && Takes(1920, 1200) && Takes(1280, 720));
    check("the size that reset the Frame is not, nor any width between two steps",
          !Takes(1366, 768) && !Takes(854, 480) && !Takes(674, 1200) && !Takes(1282, 720) && !Takes(1279, 720) && !Takes(1344, 768));
    bool steps = true;
    for (int w = -256; w <= 9000; ++w) steps = steps && Takes(w, 720) == (w >= 128 && w <= 8192 && w % 128 == 0);
    check("across: 128 to 8192, in steps of 128, and nothing else", steps);
    bool downs = true;
    for (int h = -256; h <= 9000; ++h) downs = downs && Takes(1280, h) == (h >= 128 && h <= 8192 && h % 2 == 0);
    check("down: 128 to 8192, even", downs);
    check("an Open at a width that isn't a step is refused, and says why", opened(1366, 768) == 'r' && opened(854, 480) == 'r' && opened(0, 0) == 'r');
    check("...before the device is asked anything; one at a step goes on to it", opened(1280, 720) == 'd' && opened(1408, 792) == 'd');
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
