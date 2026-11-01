// Stereo wallpapers (screens/wallimage.h, DEE-317): reading Display Settings' PPMs, and the key
// test that says where the picture goes, the same sums as the shader's (handrender.cpp kKeyed).
// Run from the repo root (it reads screens/handrender.cpp).
#include "../wallimage.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }

static bool parse(const std::string &s, wallimage::Image *im) {
    return wallimage::ParsePpm(reinterpret_cast<const uint8_t *>(s.data()), s.size(), im);
}

int main() {
    wallimage::Image im;
    const std::string px("\xff\x00\xff\x01\x02\x03", 6);
    check("a 2x1 P6 reads, top row first", parse("P6\n2 1\n255\n" + px, &im) && im.w == 2 && im.h == 1 &&
                                                  im.rgb.size() == 6 && im.rgb[0] == 255 && im.rgb[1] == 0 && im.rgb[5] == 3);
    check("comments in the header", parse("P6 # made by Display Settings\n2 # wide\n1\n255\n" + px, &im) && im.w == 2);
    check("pixel bytes that look like whitespace stay pixels",
          parse("P6\n1 1\n255\n" + std::string("\n\n\n", 3), &im) && im.rgb[0] == '\n');
    check("trailing bytes after the pixels are ignored", parse("P6\n2 1\n255\n" + px + "xx", &im) && im.rgb.size() == 6);
    check("not P6 (P3, text)", !parse("P3\n2 1\n255\n255 0 255 1 2 3\n", &im));
    check("16-bit (maxval 65535)", !parse("P6\n2 1\n65535\n" + px + px, &im));
    check("short of pixels", !parse("P6\n2 1\n255\n" + px.substr(0, 5), &im));
    check("a side of 0", !parse("P6\n0 1\n255\n", &im));
    check("a side over 4096", !parse("P6\n4097 1\n255\n" + std::string(4097 * 3, '\0'), &im));
    check("no whitespace before the pixels", !parse("P6\n2 1\n255" + px, &im));
    check("empty", !parse("", &im));
    std::string why;
    check("a missing file says so", !wallimage::LoadPpm("/nonexistent/left.ppm", &im, &why) && why.find("can't open") == 0);

    using wallimage::Key;
    const auto k = Key(1, 0, 1);
    check("the key colour is all picture at full brightness", near(k.amount, 1) && near(k.brightness, 1));
    const auto shadow = Key(0.6f, 0, 0.6f);
    check("the key darkened by a black shadow is the picture in that shadow", near(shadow.amount, 1) && near(shadow.brightness, 0.6));
    check("near the key (8-bit rounding, 254 0 253) still keys", Key(254 / 255.f, 0, 253 / 255.f).amount > 0.99f);
    check("black (a window's text) stays", near(Key(0, 0, 0).amount, 0));
    check("deep shadow fades out: dark purple content stays", near(Key(0.2f, 0, 0.2f).amount, 0));
    check("white stays", near(Key(1, 1, 1).amount, 0));
    check("a light window's anti-aliased edge over the key (1, .5, 1) stays", near(Key(1, 0.5f, 1).amount, 0));
    check("red stays", near(Key(1, 0, 0).amount, 0));
    check("pink (1, .2, 1) stays", near(Key(1, 0.2f, 1).amount, 0));
    check("between the bands it blends", Key(1, 0.05f, 1).amount > 0.1f && Key(1, 0.05f, 1).amount < 0.9f);

    // The shader has the same sums: its thresholds are these.
    std::ifstream f("screens/handrender.cpp");
    std::stringstream src;
    src << f.rdbuf();
    const std::string s = src.str();
    const size_t at = s.find("const char *kKeyed");
    const std::string shader = at == std::string::npos ? "" : s.substr(at, s.find(")\";", at) - at);
    check("kKeyed is in handrender.cpp", !shader.empty());
    check("kKeyed: the same brightness and colour offset",
          shader.find("float s = max(c.r, c.b);") != std::string::npos &&
              shader.find("float off = max(abs(c.r - c.b), c.g);") != std::string::npos);
    check("kKeyed: the same bands",
          shader.find("(1.0 - smoothstep(0.03, 0.08, off)) * smoothstep(0.30, 0.45, s)") != std::string::npos);
    check("kKeyed: the picture darkened by the shadow", shader.find(".rgb * s;") != std::string::npos);

    std::printf(failed ? "%d FAILED\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
