#ifndef FT_SCREEN_SCALE_H
#define FT_SCREEN_SCALE_H

#include <math.h>

// Logical configure size for KWin's 120ths scale, also safe for 6.2's whole buffer scale.
static inline int ft_screen_toplevel_dim(int px, double scale) {
    if (px <= 0) return 1;
    if (!(scale > 0.0)) scale = 1.0;
    double ks = round(scale * 120.0) / 120.0;
    if (ks <= 0.0) ks = 1.0;
    int k = (int)ceil(ks - 1e-6);
    if (k < 1) k = 1;
    int n = (int)lround((double)px / ks);
    if (n < 1) n = 1;
    for (int d = 0; d <= k + 2; d++) {
        int try_n = n + d;
        if (((int)lround((double)try_n * ks)) % k == 0) return try_n;
        if (d == 0) continue;
        try_n = n - d;
        if (try_n >= 1 && ((int)lround((double)try_n * ks)) % k == 0) return try_n;
    }
    return n;
}

// A spare output's size until ft-floatd sizes it. KWin 6.2 starts a spare at the scale it saved
// for that output (a floating window's, up to 3) and gives the buffer that scale rounded up as
// its whole buffer scale. A buffer that isn't a multiple of it is a protocol error that
// disconnects KWin, and KWin then restarts into the same error. 720 x 480 is a multiple of
// every whole scale up to 6.
enum { FT_SPARE_WIDTH = 720, FT_SPARE_HEIGHT = 480 };

#endif
