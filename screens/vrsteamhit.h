// Steam's own overlays out of the way while their windows are ours (STEAM_DASH_PANEL). With a
// window of Steam's ours, Steam's overlay for it draws nothing but must stay shown (Steam paints
// only while it counts as visible: vrsteamwin.cpp), so it was an invisible panel in front of the
// tiles that caught the laser (user, 2026-11-18: "an invisible panel ... intercepting us
// interacting with displays that are behind it"; Steam's page about 1.1 m out, the tiles 2.7).
// Each is shrunk to nothing while its window is ours (steamshrink.h: still shown, so Steam still
// paints), and given its width back when it's Steam's again.
#pragma once

#include "steamwin.h"

namespace ftvr {
namespace steamhit {

// Each tick: the widths as the fleet's windows are now. `all`: every one shrunk again (Steam may
// have set its width or made a new one), and each of Steam's that isn't ours given its width back.
void Step(const steamwin::Fleet &fleet, bool all);
// None of them ours any more (ft-screens going, Steam's display gone): all given back.
void Clear();
// At start, switched on or not: every one of Steam's overlays an ft-screens that went (a crash)
// left shrunk is given its width back.
void Sweep();

}  // namespace steamhit
}  // namespace ftvr
