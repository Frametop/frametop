# NPU hand tracking (ft-npud)

ft-npud runs hand tracking's landmark model on the headset's NPU (the Snapdragon's Hexagon DSP) instead of the CPU. ft-hands asks it for each hand's landmarks at `/run/ft-npud` and runs its CPU model whenever the service isn't there or a request fails, so a headset without it tracks as before ([hands/README.md](../hands/README.md), `HANDS_NPU`). With it, the landmark model's work moves off the CPU, which VR needs. It's recommended: `install.sh` and the Frametop Installer offer it with hand tracking, and Frametop Settings' Hands page says when hand tracking uses it.

```
npu/install.sh            # build the tree (a clone; a release brings it built) and install it (sudo)
npu/install.sh status     # what's installed, checked against its MANIFEST (no sudo)
npu/install.sh uninstall  # (sudo)
journalctl -u ft-npud     # "self-test ok" once it serves
```

`--no-npu` (or `--no-hands`) keeps `install.sh` from offering it. `uninstall.sh` removes it with the rest of Frametop.

## What runs

ft-npud is a root system service: the DSP's device, `/dev/cdsp`, is root's, and it works with SteamVR's `dsp_service` (from `deckard-steamvr-rel`), which runs as root. It serves one user, the one who installed it, through `/run/ft-npud`, and one client at a time; ft-hands lets go of it while hand tracking is parked or lent, so the Hand Recorder's tracker gets it. Its unit, [ft-npud.service](ft-npud.service):

- starts after `dsp_service` and, if the DSP isn't ready yet at boot or `dsp_service` restarts, tries again from 5 s to 2 min apart, for as long as it takes (it never gives up for the boot);
- runs the tree's own CPython 3.14 and numpy, not the host's `python3`, so a SteamOS update that changes the host's Python can't stop it;
- runs a self-test on the DSP before it serves, checked bit for bit against the numbers the CPU gets.

## The tree

`npu/build.sh` builds `npu/build/tree` (in the dev container; the image build runs it too, so a release brings it built):

| Part | What | From |
| --- | --- | --- |
| `app/npud/` | the daemon: `hnet_serve.py` serves the flat H-net, `trust.py` checks the tree | npu-hands and npu-rethink (`70f7f2a`), vendored here |
| `app/probe/`, `app/spikes/` | the host-side planner and the DSP programs' builders the daemon imports | the same |
| `app/framenpu/` | the DSP driver library (MIT, [its licence](app/framenpu/LICENSE), [notices](app/framenpu/THIRD_PARTY_NOTICES.md)) | NatTheDyke/framenpu `8c9baee`, vendored whole (its tests are imported at run time) |
| the four `.so` helpers | built from `app/npud/deguard.c`, `ticks.c`, `v30a_hostc.c` and `app/probe/hostc.c`, for `armv8.2-a` | `npu/build.sh` |
| `python/` | CPython 3.14.10 (python-build-standalone `20261026`) with numpy 2.5.5, without its tests, Tk, IDLE and pip | downloads pinned by sha256 in `npu/build.sh` |
| `graphs/landmarks.npz` | the model it serves: hdq, the newest tested on the headset | [graphs/hdq.npz](graphs/hdq.npz), sha256 `4625773b...b7` in `npu/build.sh` |

`npu/app` is kept as its upstreams have it, so `ruff` leaves it out (`pyproject.toml`).

## Security

Everything ft-npud runs is root's and writable by root only, from `/` down: the unit's `ExecStartPre` checks `trust.py`'s own path with the host's `python3 -I -S -B`, then `trust.py` checks the whole tree, and after its imports the daemon checks every file it loaded (`--require-root-owned`). `npu/root.sh`, the root half of `npu/install.sh`, puts the tree in place:

- The tree lives in SteamOS's root-owned offload area on the home partition, `/home/.steamos/offload/var/lib/ft-npud` (`/var` is too small for two copies), and the unit binds it read-only at `/var/lib/ft-npud`, the path the unit names. `/home` and `/etc/systemd/system/*.service` survive SteamOS updates.
- Root never opens a file in the user's tree: the tree's owner packs it (links followed, with the owner's own permissions), and root unpacks that into a stage only root can enter, so a link to `/etc/shadow` in the tree can't make root copy it out.
- A new tree is built beside the live one, chowned to root, checked by the same preflight, then swapped in; the one before stays at `ft-npud.prev`. `MANIFEST` lists every file's hash, so `status` sees a change.
- The unit's sandbox: `ProtectHome`, `ProtectSystem=strict`, `PrivateTmp`, `NoNewPrivileges`, and only `/run/ft-npud` writable.

## After a SteamOS update

ft-npud depends on `dsp_service`'s socket and the DSP firmware SteamVR brings (`/opt/steamvr/drivers/cv/bin/linuxarm64/cdsp_sm8650.mbn`), neither documented. `scripts/update-check.py` says when that firmware differs from the one it was tested with, or the service isn't running; its self-test line in `journalctl -u ft-npud` says whether it still works.
