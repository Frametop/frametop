# Dashboard tiling: where it stands (2026-11-18)

A snapshot of the `dashboard-tiling` branch for picking the work back up. The design itself is in
[design.md](design.md); this file says what's built, what's proven, and what still needs a headset.

## State

| | |
| --- | --- |
| Branch | `dashboard-tiling` in `~/frametop-wt/dashboard-tiling` |
| Merged into | local `experimental` (merge commits, one per round) |
| Installed | fast-installed on the Frame; ft-screens restarted 2026-11-18 16:49 |
| Remote | nothing pushed; no PR (Frametop pushes wait for "let's push to remote") |
| Review | council paused while the user iterates; run it before calling the branch done |
| Live test setting | `STEAM_DASH_PANEL=1` in `~/.config/frametop.conf` (backup `~/.config/frametop.conf.bak-steamdash`). Turn it off, or restore the backup, when testing ends: left on, a stale Steam panel can block input |

## What the branch does

- **Anchored tiles.** Panels anchor to the SteamVR dashboard and lay out in columns on a sphere
  around the user, packed beside Steam's own window, which shows as a tile with its menu sidebar
  glued on.
- **Stay with the dashboard closed.** `DASH_TILE_STAY` (default on) keeps anchored panels shown
  when the dashboard closes, since a click in the room closes it. A long press on a panel's anchor
  button sets that panel the other way. Steam's tile and sidebar always hide with the dashboard,
  because Steam paints them only while its tab shows.
- **Theater.** A Steam game's panel has a theater button and a View in Theater menu row. With a
  game in SteamVR's theater, the dashboard opens on Steam's tab and the tiles show over the game.
  With the dashboard closed they hide, unless `DASH_TILE_STAY_THEATER` is on (default off).
- **Gamepad pill.** The desktop pill uses SteamVR 2.19's diminished Gamepad mode with a card on
  Frametop's tab, and falls back to Steam's tab if SteamVR doesn't hand over focus within 2 s.
- **Steam sidebar.** ft-screens takes the menu window Steam shows (checked every second), not
  resizable by hand. It slides open on hover and shrinks only after Steam has shut its labels
  (`kShutWaitMs` = 2.5 s at most) so the icons are back before it narrows.
- **Game panels.** Clicks reach a game because ft-screens hands it gamescope's focus when the
  laser first comes onto its panel.

## Proven on the testbench (nobody wearing the headset)

- Dashboard closed: screen 1 and the linuxpc display stay; Steam's tile and sidebar hide.
  `DASH_TILE_STAY` 0 and 1 hide and show them live; a per-panel override hides only that panel.
- Sidebar: Steam shut its labels at 3839 ms and the crop began shrinking at 3936 ms.
- Theater: a game goes in and comes back; the dashboard opens on Steam's tab over it.
- Unit tests: dashtiles, steamglue, steamwin, tiling, vrcmd, gsapps, gstheater, dashanchor,
  barside, steamshrink, steamplace and the Display Settings tests all pass.

## Still to test in the headset

Steps for each are in `~/Desktop/Projects/HEADSET-TESTS.md`, newest first.

1. **Tiles stay, theater switch, sidebar shut** (~3 min). Click in the room: panels stay. Point at
   the sidebar and away: icons come back before it narrows. A game in the theater: panels hide,
   then stay with "In the theater" on. Stay Shown off in one panel's bar menu: only that panel hides.
2. **Steam sidebar, its corners, the front tile's height** (~2 min). Sidebar icons show, no corner
   glow or resize from the sidebar, front tile clear above the bar (say if higher or lower).
3. **Gamepad mode round 2, steps 4-7** (desktop pill): the dashboard dims with the card, the pad
   drives the desktop, a grip ends it.
4. **View in Theater** and **clicks reach a game's panel**, if not already covered by the rounds
   above.

Not driven on the testbench at all: the theater switch, the long press, the laser on SteamVR's
Return to Dashboard strip, and the dimmed gamepad path with real controllers awake.

## Next

- User headset results for the list above, then fixes.
- Council review of the branch once the user stops iterating.
- `STEAM_DASH_PANEL` is a Display Settings switch now (Steam's window, off by default, 2026-11-19); decide whether it becomes the default.
