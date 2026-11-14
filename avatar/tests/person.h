// A made-up skeleton for the tests of what poses and draws a body (bodypose-test.cpp,
// bodyview-test.cpp).
#pragma once

#include "../bodyfile.h"

// Someone 1.7 m tall in a T pose, by bodyfile's rules: feet on y = 0, facing -z, right hand at +x.
inline bodyfile::Body person(bool all = true) {
    using namespace bodyfile;
    Body b;
    auto bone = [&b](int role, double x, double y, double z) {
        b.present[role] = true;
        b.at[role][0] = float(x), b.at[role][1] = float(y), b.at[role][2] = float(z);
    };
    b.eyes[1] = 1.6f, b.eyes[2] = -0.08f;
    bone(kHips, 0, 0.95, 0), bone(kSpine, 0, 1.05, 0.01), bone(kHead, 0, 1.53, 0);
    if (all) bone(kChest, 0, 1.18, 0.02), bone(kUpperChest, 0, 1.3, 0.02), bone(kNeck, 0, 1.45, 0.01);
    for (int side = 0; side < 2; ++side) {
        const double x = side ? 1 : -1;
        const int arm = side ? kRightShoulder : kLeftShoulder, leg = side ? kRightUpperLeg : kLeftUpperLeg;
        if (all) bone(arm, x * 0.05, 1.42, 0);
        bone(arm + 1, x * 0.17, 1.42, 0), bone(arm + 2, x * 0.45, 1.42, 0), bone(arm + 3, x * 0.70, 1.42, 0);
        bone(leg, x * 0.09, 0.90, 0), bone(leg + 1, x * 0.09, 0.50, 0), bone(leg + 2, x * 0.09, 0.08, 0);
        if (all) bone(leg + 3, x * 0.09, 0.02, -0.12);
        // the thumb out ahead, then the index finger to the little one, front to back
        for (int f = 0; all && f < 5; ++f)
            for (int k = 0; k < 3; ++k)
                bone((side ? kRightFingers : kLeftFingers) + f * 3 + k, x * (f ? 0.78 + 0.03 * k : 0.72 + 0.02 * k), 1.42, f ? 0.015 * f - 0.04 : -0.03 - 0.02 * k);
    }
    return b;
}

