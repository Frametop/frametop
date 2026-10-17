// Spotwatch's reserved spot (user, 2026-10-17: "spotwatch should be a reserved spot and should
// ideally always display through displays"). Its search opens kDistance straight ahead, nearer than
// the lazy susan's front slot, which ft-screens keeps at least kFrontGap behind it: a brought panel
// never lands in the search's plane (both were 1 m ahead). Panels nearer than it (grab that, a
// carry) still don't cover it: its sort order is over theirs (ft-spotpanel.cpp).
#pragma once

namespace spotreserve {
constexpr double kDistance = 0.75;   // m ahead of the head
constexpr double kWidth = 0.42;      // m: as wide as it looks (31 degrees) as 0.56 m was at 1 m
constexpr double kFrontGap = 0.15;   // m: the front slot is at least this far behind it
}  // namespace spotreserve
