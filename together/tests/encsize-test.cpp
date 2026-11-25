// The sizes the Frame's encoder is given (together/media/irisenc.h): a width it would take the
// Frame down with is refused before the device is touched. The device here is /dev/null, so
// the test says the same on a machine with the encoder and on one without.
#include "../media/irisenc.h"

#include <linux/videodev2.h>

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
    // NV12's layout, as the 0.5.10 driver sizes it (v4l2-ctl --set-fmt-video-out, 2026-11-25)
    check("an NV12 picture's bytes are the driver's",
          irisenc::Nv12Size(640, 360) == 368640 && irisenc::Nv12Size(1280, 720) == 1413120 && irisenc::Nv12Size(1920, 1200) == 3502080 &&
              irisenc::Nv12Size(640, 1136) == 1105920);
    check("...its Cb,Cr rows after Y rows to a step of 32", irisenc::Nv12Chroma(640, 360) == 640 * 384 && irisenc::Nv12Chroma(1920, 800) == 1920 * 800);
    using irisenc::Format;
    const uint32_t ab24 = V4L2_PIX_FMT_RGBA32, nv12 = V4L2_PIX_FMT_NV12, q08c = v4l2_fourcc('Q', '0', '8', 'C');
    check("a driver that lists RGBA is given it, wherever it lists it",
          irisenc::Picks({nv12, q08c, ab24}) == Format::kRgba && irisenc::Picks({ab24, nv12}) == Format::kRgba);
    check("one with NV12 and no RGBA is given NV12", irisenc::Picks({nv12, q08c}) == Format::kNv12 && irisenc::Picks({q08c, nv12}) == Format::kNv12);
    check("one that lists neither is asked for RGBA", irisenc::Picks({}) == Format::kRgba && irisenc::Picks({q08c}) == Format::kRgba);
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
