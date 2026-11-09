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

#endif
