// See image.h.
#include "image.h"

#include <cstdio>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 8192
#include "stb_image.h"

namespace importer {

bool DecodeImage(const uint8_t *data, size_t size, int maxSide, Image &out) {
    out = Image();
    if (!data || size == 0 || size > size_t(1) << 30) return false;
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(data, int(size), &w, &h, &channels) || w < 1 || h < 1) return false;
    stbi_uc *pixels = stbi_load_from_memory(data, int(size), &w, &h, &channels, 4);
    if (!pixels) return false;
    out.width = w, out.height = h;
    out.rgba.assign(pixels, pixels + size_t(w) * size_t(h) * 4);
    stbi_image_free(pixels);
    while (out.width > maxSide || out.height > maxSide) Halve(out);
    return true;
}

bool PlainFileName(const char *name) {
    return name && *name && !std::strchr(name, '/') && !std::strchr(name, '\\') && !std::strstr(name, "..") && !std::strchr(name, '%') && !std::strchr(name, ':');
}

bool DecodeImageFile(const std::string &folder, const char *name, int maxSide, Image &out) {
    constexpr size_t kMaxFile = 64u << 20;
    if (!PlainFileName(name)) return false;
    FILE *f = std::fopen((folder + "/" + name).c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && bytes.size() <= kMaxFile) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return bytes.size() <= kMaxFile && DecodeImage(bytes.data(), bytes.size(), maxSide, out);
}

void Halve(Image &im) {
    const int w = im.width > 1 ? im.width / 2 : 1, h = im.height > 1 ? im.height / 2 : 1;
    if (w == im.width && h == im.height) return;
    std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned sum[4] = {}, byAlpha[3] = {}, n = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = 2 * x + dx, sy = 2 * y + dy;
                    if (sx >= im.width || sy >= im.height) continue;
                    const uint8_t *p = &im.rgba[(size_t(sy) * size_t(im.width) + size_t(sx)) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    for (int c = 0; c < 3; ++c) byAlpha[c] += unsigned(p[c]) * p[3];
                    ++n;
                }
            }
            uint8_t *o = &out[(size_t(y) * size_t(w) + size_t(x)) * 4];
            for (int c = 0; c < 3; ++c) o[c] = uint8_t(sum[3] ? (byAlpha[c] + sum[3] / 2) / sum[3] : (sum[c] + n / 2) / n);
            o[3] = uint8_t((sum[3] + n / 2) / n);
        }
    }
    im.width = w, im.height = h;
    im.rgba = std::move(out);
}

}  // namespace importer
