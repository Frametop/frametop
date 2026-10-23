# Personal fork: recenter countdown and controls guide

This branch is based on upstream `v0.3.0-exp.19` (`ef97943247de10e840f9b6759cc5a330bc761082`). Upstream remains [Frametop/frametop](https://github.com/Frametop/frametop); the maintained customization is [LeeroyDing/frametop, branch personal](https://github.com/LeeroyDing/frametop/tree/personal).

## Use

Open **Frametop Controls Guide** from the desktop application menu, or **Controls guide** in Frametop Input Settings. It opens the **Recenter & controls** reference page in the existing Frametop Tour. You can also choose that page from the Tour's page list. It works offline and does not perform a reset when opened.

For screen reset, aim at the target-shaped icon to the left of a screen's grab bar, pinch once, release, and look forward. A head-following **3 → 2 → 1** says **Look straight ahead**, then the existing layout reset samples the current head pose. Meta+Shift+R, the Reset Screen Layout menu item, and mapped reset controls use the same countdown. A direct fingertip tap retains its confirmation step first.

With the current ft-screens desktop, Reset moves existing unpinned screens around your current head pose while preserving application windows, fullscreen, display settings, and remote connections. Pinned, off, and missing screens are skipped; independent floating windows stay in place. Use Open profile explicitly to restore saved apps and window positions. The older gamescope backend retains full-profile reset behavior. Pointer Recenter is a different action from resetting the screens, and Lazy Susan rotation itself is unchanged.

## Source and installation

Clone this branch explicitly; the upstream `get.sh` URL and upstream release downloads do not install a fork:

```sh
git clone --branch personal https://github.com/LeeroyDing/frametop.git
cd frametop
```

For a new source installation, follow the repository's normal installation prerequisites and run `./install.sh` interactively. It builds the countdown with the existing components and installs the guide launcher. Review the installer's optional components; a full install can require a SteamVR restart. A prepared release image builds the same helper through `pack/Containerfile`; this fork does not currently publish prebuilt release assets.

For an existing installation, stage the changes against its exact release and preserve its configuration, local patches, and original files. Avoid replacing a running installation wholesale. The guide itself consists of the Tour page, its launcher, and the Input Settings link. The countdown is a separate source/build integration; an existing verified local countdown can remain in place while adding the guide.

- `tour/ft_tour.py`: reference page data; `FT_TOUR_PAGE=recenter` deep-links it.
- `tour/ft-recenter-help.desktop` and `tour/install.sh`: direct application-menu entry.
- `input-settings/`: opens the reference page from the settings menu.
- `layout/countdown/`: helper source, pinned build recipe, and offline tests; output stays in its ignored `build/` directory.
- `layout/ft_layout.py`: countdown before current-head sampling, geometry-only recentering on ft-screens, and a separate nonblocking lock held through reset completion.

The helper is optional. If it cannot run, reset retains a minimum three-second delay; a hung helper is bounded. It accepts no input and uses the `frametop.guide` overlay-key prefix excluded by the custom pointer. Direct `apply`, `use`, and startup operations retain their original timing.

## Verify changes

Run `python3 tour/test/tour-test.py` for the existing Tour suite and the offline countdown checks documented in `layout/countdown/README.md`. The normal `just test` recipe includes these checks in the build container. Check Python/shell syntax and load the guide using the matching Qt/Kirigami runtime.

Build with `layout/countdown/build.sh` through the normal container workflow, then run the built helper's `--check` offline in the runtime where reset executes. Inspect its offline `--render-dir DIR` previews if its appearance changes. A deliberate live reset test should confirm the countdown follows the head, the pointer passes through it, it disappears, and the final alignment is correct. Reset on ft-screens should leave playback fullscreen and app/window state intact; explicit Open profile can reposition application windows.

## Keep the fork up to date

Keep `upstream` pointing to `https://github.com/Frametop/frametop.git`. Review upstream changes before merging or rebasing them into `personal`; the base here is experimental, while upstream `main` can lag it. If upstream adds equivalent countdown/help behavior, reconcile or retire the local changes rather than stacking duplicate delays.

After each update, check reset dispatch and current-head sampling, profile behavior, the separate duplicate-reset lock, the helper's container path and fonts, and exclusion of `frametop.guide` overlays from pointer targeting. Run the tests against the resulting code and do a controlled in-headset check. Neither a clean Git merge nor an old test result proves a new runtime works.

For rollback, restore the previous source revision or the same installed release's pre-edit files and helper. Removing only the helper leaves the Python fallback delay. Do not copy the complete exp.19 layout script over a newer release. The upstream uninstaller removes the new guide launcher together with the existing Frametop entries.
