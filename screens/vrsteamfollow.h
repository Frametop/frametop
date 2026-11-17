// Steam's popups (steamwin.h's followers: the footer, tooltips, the keyboard, notifications) as
// bare overlays of ours, gamescope drawing each one's window: shown while Steam shows its own one,
// where Steam puts it, moved with the tile it sits on (steamplace.h), and the laser on them as their
// pages' mouse (steamcdp.h). vrsteamwin.cpp makes and drops them as steamwin::Fleet says; Step does
// the rest.
#pragma once

#include "gsforward.h"
#include "steamcdp.h"
#include "steamwin.h"

#include <cstdint>

namespace ftvr {
namespace steamfollow {

constexpr int kTileIndex = 189;   // the tiles' panels: screen 190 + their slot (vrsteamwin.cpp)

uint64_t Make(const gsforward::Window &w, uint32_t gamescopePid);   // hidden until Step shows it; 0: none
void Size(uint64_t overlay, const gsforward::Window &w);
void Drop(uint64_t overlay);
// Each tick: each one placed and shown, or hidden; their laser events to their pages.
void Step(const steamwin::Fleet &fleet, steamcdp::Pages &pages);

}  // namespace steamfollow
}  // namespace ftvr
