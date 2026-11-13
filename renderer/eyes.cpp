#include "handcut.h"

namespace handcut {
void EyePositions(const Mat &head, double out[2][3]) {
    const vr::EVREye eyes[2] = {vr::Eye_Left, vr::Eye_Right};
    for (int e = 0; e < 2; ++e) {
        const Mat t = vr::VRSystem()->GetEyeToHeadTransform(eyes[e]);
        for (int i = 0; i < 3; ++i)
            out[e][i] = head.m[i][0] * t.m[0][3] + head.m[i][1] * t.m[1][3] +
                        head.m[i][2] * t.m[2][3] + head.m[i][3];
    }
}
}  // namespace handcut
