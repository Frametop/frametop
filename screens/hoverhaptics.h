// One physical-controller pulse per hover entry/source change, never a broadcast.
#pragma once

namespace hoverhaptics {
struct Gate {
    int device = -1;
    bool Enter(int source, bool physicalController, bool enabled) {
        if (!physicalController) { device = -1; return false; }
        const bool pulse = enabled && source != device;
        device = source;
        return pulse;
    }
    void Leave(int source = -1) { if (source < 0 || source == device) device = -1; }
};
}  // namespace hoverhaptics
