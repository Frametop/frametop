# G7 Build and release

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


Covers how a build is made and how a release reaches the Frame. On the build side: the dev box (`setup/dev-container.sh`, a Fedora 44 toolbox in distrobox), a source install's `install.sh` in a clone, `ft dev build` and `ft dev test` (the image from `pack/Containerfile`, with its pinned base, `uv.lock`, OpenVR and `.dockerignore`), CI (`.github/workflows/release.yml` on a `v*` tag or a manual run, with its version rule and smoke test), and `framedrop/build.sh` with `pack/release-info.py` (the OCI archive, `frametop-release.json`, `Frametop.zip` and its manifest). On the install side: the ways in (`get.sh` and its channels, FrameDrop's installer and askpass socket, the zip by hand, or from source), `pack/install-release.sh` (the SteamOS table, the image load and sha256 check, the atomic copy into `releases/VERSION`, `current` and the clean-up), `install.sh`'s 12 steps under `set -e`, `scripts/fast-install` and its undo, the models (the pinned hand student, `ft-models` with the hands and eyes catalogs, `voice/models.sh`), and upkeep (`update-check.py`, `conf-migrate.sh`, `uninstall.sh`). Arrows into G1, G3, G4, G5 and G8 show where an install's units and models reach those groups. The flow is drawn by hand. The `.mmd` names the files and lines.

Model: `G7-release.tla` (module `G7Release`). It follows what an install leaves behind, release after release (N = 3): each release's folder and container, the release each unit points into (core for the units always installed, hands for an optional part), the `current` link, a stopped `install.sh`, and `fast-install` patches. Configs: `Code` (every fault on), `CIOnly` (CI releases and full installs only, no `fast-install`), `Skips` (a later install skips the optional part), `Rollback` (an older release's `install.sh` run to go back, with `fast-install`), `Interrupt` (one `install.sh` stops mid-way, with `fast-install`), `Local` (local builds), `Reinstall` (the same release installed again after a `fast-install`), `Fixed` (all four fixes, no interruption) and `FixedInterrupt` (all four fixes with an interruption).

Results at `78e93b1`:

| Rule | What it says | Result |
| --- | --- | --- |
| TypeOK | | Holds in all 9 configs |
| UnitsHaveFolder | Every unit points into a release folder and container that are still there | Breaks in Skips, 10 steps (W2) and Code. Holds in every other config |
| NoMixedUnits | Outside a running install, every unit points into one release | Breaks in Skips, 7 steps (W2), Interrupt, 7 steps (W5), FixedInterrupt and Code |
| CurrentIsLive | Outside a running install, `current` names the release the units run | Breaks in Rollback, 12 steps (W1), Interrupt, 3 steps (W5), FixedInterrupt and Code |
| PatchLands | `fast-install` never reports success for a tree nothing runs | Breaks in Rollback, 13 steps (W1), Interrupt, 8 steps (W5), FixedInterrupt and Code |
| ReleaseMatchesCommit | Every release's image was built from the commit its release JSON names | Breaks in Local, 1 step (W3) and Code. Holds in CIOnly and Fixed |
| ReinstallGivesRelease | Installing a release again gives that release as it was built | Breaks in Reinstall, 9 steps (W4), Rollback, Interrupt and Code |
| InstallSettles | A stopped install is finished by running the same command again | Holds in all 9 configs |

CIOnly (116 states) and Fixed (1,844) hold every rule. States per run: 116 (CIOnly) to 155,405 (Code). FixedInterrupt shows that none of the W1 to W4 fixes helps W5: only running the command again settles it.

Also checked in the code and drawn as holding: the copy out of the image is atomic (`VERSION.new`, then `mv`), a damaged image file stops before anything is written (exit 3), another image under a version already installed is refused, and every model file is pinned to a commit and a sha256.

Results with the fixes (`140aced`): 72 runs. Every rule holds in CIOnly, Local, Skips, Rollback, Reinstall and Fixed. Code, Interrupt and FixedInterrupt still break NoMixedUnits (7 steps), CurrentIsLive (3 steps) and PatchLands (8 steps), all on W5's path; UnitsHaveFolder, ReleaseMatchesCommit and ReinstallGivesRelease now hold there. States per run: 116 (CIOnly, Local) to 15,049 (Code, FixedInterrupt). The model now describes the fixed code: a tree's `install.sh` points `current` at itself when it finishes (W1), a part an install skips has its units removed when they point into another release (`drops`, W2), a local pack can't make a release from a changed tree, so `LocalBuild` adds no states (W3), and a folder with fast-installed files can't be installed over (W4), so a re-run after an interruption comes after a `fast-install undo`. `FixCurrent`, `FixSkip`, `FixReinstall` and `FixLocal` are no longer used, so Code and FixedInterrupt check the same code. `FixSkip` and `FixReinstall` were different fixes (renew the skipped part's units, copy the folder again); the code took the ones that change less (below). `framedrop/build.sh --allow-changes`, for local test releases, isn't modelled. To see the old breaks, check out `f85dff9` and run its copy of the model.

Undoing a fix in a copy of the model brings its break back: with a tree's `install.sh` leaving `current` alone, CurrentIsLive (12 steps) and PatchLands (13 steps) break in Rollback. Keeping a skipped part's units in another release, UnitsHaveFolder (12 steps) and NoMixedUnits (8 steps) break in Skips. With a changed tree packed, ReleaseMatchesCommit breaks in Local (1 step). Without the refusal, ReinstallGivesRelease breaks in Reinstall (9 steps), and with a re-run that skips the undo it breaks in Interrupt and FixedInterrupt (11 steps).

`pack/test/install-release-test.py` checks W1 and W4 in `install-release.sh` offline, and that the release `current` named before is still kept now that `install.sh` moves the link first. It fails on the old `install-release.sh`. W2 needs a Frame, so it has no offline test. W3 was checked with a stub podman: a clean tree at HEAD packs, a changed tree, another commit or an image with no labels is refused, and `--allow-changes` warns and packs. CI builds with `./ft dev build` from a clean checkout and passes `--commit "$GITHUB_SHA"`, so its images pass.

The step counts above are TLC's for every break at `78e93b1`, the old code, run from a copy of the model. Running the check here writes the fixed model's traces to `G7-release-traces/`, which git ignores like the other groups' folders. The chart's W2 and W4 legend boxes say 9 and 8 steps, one short.

The chart (`G7-release.mmd`, `.svg`) still draws W1 to W4 red. It is due a redraw.

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| W1 | DEE-286 | CurrentIsLive, PatchLands | Only `install-release.sh` moved `releases/current`, after `install.sh`. Going back by running the older release's `install.sh`, as `pack/README.md` and `install-release.sh` said, moved the units and left `current` on the newer release. `fast-install` then patched `current`'s tree, found no unit in it or restarted units that ran the other tree, and reported success. The next install kept the wrong release as the one before, and `get.sh` read the channel from it | Fixed in `140aced`. A release's `install.sh` points `current` at its own folder when it finishes (only a release folder named for its `VERSION`). `install-release.sh` reads the release `current` named before it runs `install.sh`, so the release before is still kept |
| W2 | DEE-287 | UnitsHaveFolder, NoMixedUnits | A part a later install skipped (voice or hands unticked, `--no-voice`, `--no-hands`, or `--yes` with no quiet sudo) kept its units in the old release's folder and container. Two releases on, `install-release.sh` removed that folder and container, and those services failed | Fixed in `140aced`. When an install skips voice or hand tracking, or its install fails, that part's units that point into another Frametop folder are stopped and removed (with the `ft-handsctl` link), and the message says how to install it for this folder. Units already in this folder stay. The Hand Recorder has its own ft-camd unit, so it is unaffected |
| W3 | DEE-288 | ReleaseMatchesCommit (local builds only) | `ft dev build` built the image from the working tree, uncommitted and untracked files in, and `framedrop/build.sh` named HEAD at pack time as its commit. `fast-install` diffs from that commit and trusts it for its rebuilds | Fixed in `140aced`. `ft dev build` labels the image with its commit and whether the tree had changes (`org.frametop.commit`, `org.frametop.tree`). `framedrop/build.sh` refuses an image built from a changed tree, from a commit other than `--commit`, or with no labels. `--allow-changes` packs it anyway, with a warning |
| W4 | DEE-289 | ReinstallGivesRelease | Installing the same release again skipped the copy when its folder was there, so fast-installed files and `.fast-install/` stayed in a tree that named the release's commit | Fixed in `140aced`. `install-release.sh` refuses to install over a folder with `.fast-install/state.json`, and says to run `scripts/fast-install undo` first (after that release's `install.sh`, if another one is current) |

Open suspected bugs:

| Id | Ticket | Rule | What happens | Why it's still open |
| --- | --- | --- | --- | --- |
| W5 | [DEE-291](https://linear.app/deejanus/issue/DEE-291) | NoMixedUnits, CurrentIsLive, PatchLands (minor) | `install.sh` runs 12 steps under `set -e`. A failing step or a power-off leaves the early steps' units in the new release and its container, the later ones in the old release, and `current` on the old one. `fast-install` then patches the old tree. Running the same command again finishes it | Documented, not fixed. A safe fix needs a transactional install (every unit written for the new release first, then one switch), which reworks all 12 steps and can't be checked without running an install. `install.sh` now says "Run the same command again to finish the install" when it stops early, a signal included (Ctrl-C, a closed terminal, FrameDrop stopping it: `scripts/tests/test_install_interrupt.py`), its end message says it too, and `pack/README.md` (step 6), `README.md` (Update) and `install-release.sh`'s header say the same |

Way around it until it is fixed: run the same command again.

Design choices in these fixes (Daenon can override each):

- W1: only a release's `install.sh` moves `current`, and only when its folder's name is its `VERSION`. A clone's `install.sh` leaves the link alone, since `current` must name a release folder.
- W2: a skipped part's units from another folder are stopped and removed, not renewed for this folder (that would install a part the person said no to, or one that needs sudo) and not kept with their old folder (that would leave units across releases and a folder that has to outlive its release). The part stays off until it is installed again, and the message says how.
- W3: refused by default, with `--allow-changes` as the way out for local test releases. An image with no labels (built before this change, or by another tool) is refused too: rebuild it with `ft dev build`, or pass `--allow-changes`.
- W4: a reinstall over fast-installed files is refused rather than copied again. Copying again would quietly drop the fast-installed commits, and a fresh copy loses ft-camd's file capabilities and the tree's own models until `install.sh` sets them again.
- W5: documentation and an exit message only (see above).
