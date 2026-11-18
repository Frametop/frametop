// The width Steam's own overlay gets while its window is ours (vrsteamhit.cpp): a ten-thousandth
// of its own, so it's gone (nothing to catch gaze or hands; the laser takes a mask too, vrsteamhit.cpp) but still shown, and Steam
// keeps painting the window (it paints only while that overlay counts as visible; a tiny one
// does, seen 2026-11-18: 90 frames a second at 1 mm). The shrunk width is also the record of the
// real one: no file, and an ft-screens that crashed leaves nothing a new one can't put back.
// Steam's own widths are 0.3 m and up, so anything under a millimetre is ours.
//
// Pure math, no OpenVR: screens/tests/steamshrink-test.cpp.
#pragma once

namespace steamshrink {

constexpr float kScale = 1e-4f;   // shrunk = its own width x this
constexpr float kMax = 1e-3f;     // narrower than this: shrunk by us

inline bool Shrunk(float w) { return w > 0 && w < kMax; }
// The width to set to get it out of the way, or 0: it already is (or has none).
inline float Away(float w) { return w > 0 && !Shrunk(w) ? w * kScale : 0; }
// The width to set to give it back, or 0: it isn't shrunk.
inline float Back(float w) { return Shrunk(w) ? w / kScale : 0; }

}  // namespace steamshrink
