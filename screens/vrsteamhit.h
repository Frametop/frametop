// Steam's own overlays kept out of the laser's way while their windows are ours (STEAM_DASH_PANEL).
// With a window of Steam's ours, Steam's overlay for it draws nothing but stays shown (Steam paints
// only while it counts as visible: vrsteamwin.cpp), so it would still catch the laser in front of
// the tiles (user, 2026-11-18: "an invisible panel ... intercepting us interacting with displays
// that are behind it"; Steam's page about 1.1 m out, the tiles 2.7). Each gets an intersection
// mask that matches nothing while its window is ours, cleared when it's Steam's again.
#pragma once

#include "steamwin.h"

namespace ftvr {
namespace steamhit {

// Each tick: the masks as the fleet's windows are now. `all`: every one set again (Steam may have
// made its overlays again), and cleared on each of Steam's that isn't ours.
void Step(const steamwin::Fleet &fleet, bool all);
// None of them ours any more (ft-screens going, Steam's display gone): all cleared.
void Clear();
// At start, switched on or not: cleared on Steam's fixed-key overlays, in case an ft-screens that
// went (a crash) left one masked.
void Sweep();

}  // namespace steamhit
}  // namespace ftvr
