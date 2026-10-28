# Working on Frametop

Rules for people and coding agents changing this repo. The README covers what Frametop is and how to install it; `docs/reference.md` covers each component, and `docs/design.md` records how SteamVR on the Frame behaves and why things are built the way they are. Read its notes on SteamVR before changing how the screens, the pointer, or the input relay talk to SteamVR.

## Two ways to run the scripts

Every script works in both modes, and must keep working in both:

- On the Frame (SteamOS, VR variant), commands run locally, in this checkout.
- From a PC over SSH, the repo is synced to `~/dev/frametop` on the Frame (`scripts/sync.sh`), and commands run there. `scripts/_env.sh` works out which mode applies (`FRAME_LOCAL`, `FRAME_HOST`, `FRAME_REPO`).

```
scripts/sync.sh                          # PC -> ~/dev/frametop on the Frame
scripts/frame.sh -C <dir> '<build cmd>'  # runs in the "dev" Fedora distrobox
scripts/frame.sh --host '<cmd>'          # runs on the SteamOS host
```

- From a PC, edit only on the PC. The Frame's copy is a mirror that `sync.sh` overwrites.
- Build inside the `dev` container. The SteamOS host has a read-only root and no compilers. Container builds link against the container's libraries, so they run in the container (`distrobox enter dev -- ...`); the SteamVR driver is built to run on the host.
- Container packages the build needs go in the list in `setup/dev-container.sh`, so the container can be rebuilt.
- Build output goes in `build/` next to the sources. It's gitignored and never synced.

## Trying a change on the headset

Commit it, then put the commit into the live release with `scripts/fast-install`. It rebuilds only the components whose sources changed since the release's commit (in the release's own container, so they link against what the release runs), puts the changed files and programs in place, and restarts the services running them, the Frametop desktop included. Python, QML and scripts take seconds, a rebuilt component a minute or two (the first build of hands, voice, stream or keyboard takes longer: it fetches and builds their libraries). `scripts/fast-install status` shows what's on top of the release, `undo` puts the release back as it was, `--dry-run` shows what would change, and `--no-restart` leaves the services alone.

- It installs commits, not the working tree: what runs is a commit you can name.
- Changes the image carries (`uv.lock`, `pyproject.toml`, `pack/Containerfile`, the container's packages) need the full build; it refuses them unless `--partial`. So does a changed ft-camd: a new one needs file capabilities. Changes an installer puts in place (units, menu entries, the SteamVR driver, `/etc` files) are copied into the tree and listed as steps left (exit 3): run that installer from the live release, then `scripts/fast-install done PATH`, or do the full build.
- The full build comes before a push: `ft dev build` and `get.sh --zip` locally, or CI for a release. That's also the only way to test the installers.
- It never restarts SteamVR. Restarting the desktop ends what's open on it, so while the headset is worn it leaves the desktop alone unless `--worn-ok`: ask the wearer first.

## The headset may be in use

A Steam Frame is someone's personal headset, and they may be wearing it while you work.

- Don't kill or restart `gamescope`, `steam`, `vrserver`, `vrcompositor`, the gamescope session, or the Frametop desktop without asking. Each one ends or disrupts whatever is happening in VR.
- Don't run host `sudo`, `steamos-readonly disable`, `steamos-devmode` changes, pacman installs, or reboots without explicit approval. Three installers need host `sudo`, and they ask for it: the Bluetooth fixes (`setup/bluetooth/install.sh`), hand tracking (`hands/run.sh install` and `caps`, which set ft-camd's file capabilities with `setcap`, and `uninstall` and `uncaps`, which take them back), and our own eye tracker's frame grabber (`gaze/tracker/install.sh`).
- Write only inside the repo, `/tmp`, and the container unless told otherwise. `scripts/fast-install` writes into the live release and `~/.cache/frametop-fast`. The installers are the exception: they write the user services, launchers, and the SteamVR driver into the home folder. The Bluetooth fixes and the eye tracker's frame grabber also install root-owned files and system services under `/etc` (`/etc/steamframe`, `/etc/frametop`, `/etc/systemd/system`). When an installer starts writing something new outside the repo, or sets file capabilities, add it to `uninstall.sh` too: users uninstall with that script, not with each installer's `uninstall`.
- The testbench (`testbench/`) is the one exception: a development tool that never ships, so `uninstall.sh` leaves it alone and `testbench/install.sh uninstall` removes it. Its preload runs inside vrserver. A SteamVR that keeps crashing makes SteamOS repair Steam after three failed sessions and fall back to the other OS slot after four, so its loader leaves the proxy out after a failed session. Restart SteamVR with the testbench only through `testbench/install.sh install|uninstall --restart`, which checks for that, and never drive the pose while someone wears the headset.
- Never copy `.netrc`, SSH keys, or Steam config off the Frame or into this repo.
- To prove a change in the headset, use the verification skill in `.claude/skills/verify-frametop/`: it drives the live Frametop through the testbench with nobody wearing the headset, and refuses otherwise.

## SteamOS updates

A SteamOS update replaces SteamVR, KWin, and gamescope with the rest of the OS image. When a change starts depending on something from the image (a host file, an OpenVR interface outside the bundled header, an undocumented layout or output format, a SteamVR or KWin quirk), add a check for it to `scripts/update-check.py`, or a retest hint for its package there. [docs/design.md](docs/design.md) has the background.

## The image (`pack/`)

`pack/Containerfile` builds Frametop as an OCI image, the groundwork for installing without building on the headset; no installer uses it yet. [pack/design.md](pack/design.md) explains why and what is open, and [pack/README.md](pack/README.md) documents the specifics. Rules:

- **Pin every input.** Base images by digest, Python via `uv.lock` (commit the lock, never a bare `uv pip install`), downloads by tag or commit and sha256, CI actions by commit SHA.
- **One build recipe.** The image runs the components' own `build.sh` scripts (`FRAME_IN_BOX=1`), and the OpenVR SDK they build against is pinned in `scripts/openvr.sh`. Change a build there, not in the Containerfile, and keep the Containerfile buildable on arm64, the only architecture the Frame has.
- **Keep the tests green in the image.** `./ft dev test` runs `just test` inside it, and CI runs the same recipes. A new offline test that needs no headset goes in the `justfile` too.

## Names

User-facing names are "Frametop", "Frametop Display Settings", and "Frametop Input Settings". Programs and files use the `ft-` / `ft_` prefix (`ft-screens`, `ft-pointer`, `ft-layout`, the `ft_pointer` driver); config, units, and overlay keys use `frametop`. Program names must stay within 15 characters: Linux truncates process names there, and the scripts find programs with `pgrep -x` / `pkill -x`.
