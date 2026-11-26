#include "../common/overlayregion.h"
#include <cassert>
#include <iostream>

int main() {
    overlayregion::Region region{{false, 0.1, 0.2, 0.3, 0.1}, {true, 0.7, 0.7, 0.1, 0}};
    assert(overlayregion::Contains(region, 0.2, 0.25));
    assert(overlayregion::Contains(region, 0.7, 0.7));
    assert(!overlayregion::Contains(region, 0.2, 0.7));
    assert(!overlayregion::FullRectangle(region));
    assert(overlayregion::FullRectangle({{false, 0, 0, 1, 1}}));
    assert(!overlayregion::FullRectangle({{true, 0.5, 0.5, 0.5, 0}}));
    overlayregion::Region image;
    for (const auto &p : region) image.push_back(overlayregion::TopLeft(p));
    assert(std::fabs(image[0].y - 0.7) < 1e-12); // rectangle origin includes its height
    assert(std::fabs(image[1].y - 0.3) < 1e-12); // circle center does not
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) {
        const double u = (x + 0.5) / 100, v = (y + 0.5) / 100;
        assert(overlayregion::Contains(region, u, v) == overlayregion::Contains(image, u, 1 - v));
    }
    std::string key;
    overlayregion::Region parsed;
    assert(overlayregion::Decode(overlayregion::Encode("frametop.screen.1.resize", region).c_str(), &key, &parsed));
    assert(key == "frametop.screen.1.resize");
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x)
        assert(overlayregion::Contains(region, x / 100.0, y / 100.0) == overlayregion::Contains(parsed, x / 100.0, y / 100.0));
    for (const char *bad : {"region native 1 1 .5 .5 .1 0", "region frametop.x 33", "region frametop.x 0",
                           "region frametop.x 1 1 .5 .5 nan 0", "region frametop.x 1 1 .5 .5 -1 0",
                           "region frametop.x 1 0 .9 .9 .2 .2", "region frametop.x 1 1 .5 .5 .1 0 extra"})
        assert(!overlayregion::Decode(bad, &key, &parsed));
    std::cout << "ok shared hit regions: exact union, wire roundtrip, malformed input rejected\n";
}
