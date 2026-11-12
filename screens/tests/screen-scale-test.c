// Keep assertions live even in release builds.
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../screen-scale.h"

int main(void) {
    assert(ft_screen_toplevel_dim(1920, 1.0) == 1920);
    assert(ft_screen_toplevel_dim(1080, 1.0) == 1080);
    assert(ft_screen_toplevel_dim(3840, 2.0) == 1920);
    assert(ft_screen_toplevel_dim(2160, 2.0) == 1080);
    assert(lround(1920 * 2.0) == 3840);
    assert(lround(1080 * 2.0) == 2160);
    assert(ft_screen_toplevel_dim(2560, 1.25) == 2048);
    assert(ft_screen_toplevel_dim(1440, 1.25) == 1152);
    assert(ft_screen_toplevel_dim(3840, 1.5) == 2560);
    assert(ft_screen_toplevel_dim(2160, 1.5) == 1440);
    assert(ft_screen_toplevel_dim(0, 2.0) == 1);
    assert(ft_screen_toplevel_dim(-10, 2.0) == 1);
    assert(ft_screen_toplevel_dim(1920, 0.0) == 1920);
    assert(ft_screen_toplevel_dim(1920, -1.0) == 1920);
    assert(ft_screen_toplevel_dim(1920, NAN) == 1920);
    assert(ft_screen_toplevel_dim(1920, 0.001) == 1920);
    const int sizes[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {3440, 1440}, {5120, 1440}};
    const double scales[] = {0.75, 1.0, 1.25, 4.0 / 3.0, 1.5, 1.75, 2.0};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        for (unsigned j = 0; j < sizeof scales / sizeof scales[0]; j++) {
            double ks = round(scales[j] * 120.0) / 120.0;
            int k = (int)ceil(ks - 1e-6);
            for (int axis = 0; axis < 2; axis++) {
                int logical = ft_screen_toplevel_dim(sizes[i][axis], scales[j]);
                assert(logical >= 1);
                assert(((int)lround(logical * ks)) % k == 0);
                assert(abs(logical - (int)lround(sizes[i][axis] / ks)) <= k + 2);
            }
        }
    }
    puts("screen-scale: ok");
    return 0;
}
