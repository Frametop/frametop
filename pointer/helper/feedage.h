// How old a file ft-hands published is (fh_hands.h, fh_gestures.h: publish_ns, CLOCK_MONOTONIC),
// seen from the pointer helper's loop. Header-only and free of OpenVR, so pointer/tests/feedage-test.cpp
// can drive it.
//
// The loop reads its clock once, at the top (tnow), and the files later in the same pass. A frame
// ft-hands writes in between carries a publish_ns after tnow. Unsigned "now - publish" then wraps to
// about 584 years: the feed read as stale for that one pass, so the handoff's sides went "off" and
// the "hands" word to ft-screens flickered to "off off" and back (24 times in 7 minutes on
// 2026-10-10, each ending whatever ft-screens held, and the ray). A frame from after "now" is age 0.
#pragma once

#include <cstdint>

namespace hin {

// publishNs's age at nowNs, in ns; 0 when it was published at or after nowNs.
inline uint64_t AgeNs(uint64_t nowNs, uint64_t publishNs) { return nowNs > publishNs ? nowNs - publishNs : 0; }

// Something was published (publishNs set) less than maxAgeNs before nowNs, or after it.
inline bool Fresh(uint64_t nowNs, uint64_t publishNs, uint64_t maxAgeNs) {
    return publishNs != 0 && AgeNs(nowNs, publishNs) < maxAgeNs;
}

}  // namespace hin
