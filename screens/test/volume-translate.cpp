// Grab ownership and pose translation, independent of camera intent.
#include "../passthrough.h"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace passthrough;
int main() {
    for (double yaw : {0., 37., 90., -155.}) {
        Volume v;
        v.yaw = yaw;
        const auto start = v;
        TranslateDrag drag;
        assert(drag.Begin(v, 4));
        assert(!drag.Begin(v, 3));
        auto pose = VolumePose(v);
        assert(drag.UpdatePose(v, pose));
        assert(!drag.Finish(v, 4));
        assert(drag.Begin(v, 4));
        const Point delta{.2, -.3, .4};
        for (int k = 0; k < 3; ++k) pose[k][3] += delta[k];
        for (int i = 0; i < 100; ++i) assert(drag.UpdatePose(v, pose));
        for (int k = 0; k < 3; ++k)
            assert(std::fabs(v.center[k] - start.center[k] - delta[k]) < 1e-9);
        assert(v.size == start.size && v.yaw == yaw && !v.enabled);
        assert(!drag.Finish(v, 3) && drag.Active());
        assert(drag.Finish(v, 4) && !drag.Active());
        assert(drag.Begin(v, 4));
        assert(drag.UpdatePose(v, VolumePose(start)));
        drag.Cancel(v);
        assert(VolumePose(v) == pose);
        assert(drag.Begin(v, 4));
        char reply[256];
        Command("passthrough set 0 1 2 3 .05 5 2 90", v, reply, sizeof reply, nullptr, &drag);
        const auto external = VolumePose(v);
        assert(!drag.Active() && !drag.UpdatePose(v, pose));
        drag.Cancel(v);
        assert(VolumePose(v) == external);
    }
    std::puts("rectangle grab: pose translation, no drift, ownership, rollback, external set");
}
