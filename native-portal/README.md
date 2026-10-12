# Native masked passthrough

SteamVR owns camera acquisition, stereo reprojection, GPU resources and submission.
This component changes camera coverage and ordering relative to overlays. It is
pinned to the tested Steam Frame compositor and eight native shaders; mismatched
binaries are rejected. Stock full-room RoomView bypasses Frametop's masks and
retains native ordering. Returning to Aurora resumes the requested masks.

## Build and run on the headset

From the checkout, generate shaders on the host and compile in the dev container:

```sh
python3 native-portal/generate.py
~/.local/bin/distrobox enter dev -- python3 native-portal/build.py
python3 native-portal/run.py enable
```

Build output has one default location: `native-portal/build/current/`. Generation
requires SPIR-V Tools; compilation requires C++17, Vulkan headers and OpenSSL.
Generated native shader bytes stay in ignored build output.

Enable restarts SteamVR and restores the running Frametop desktop and companion
services. It installs an opt-in user SteamVR override and a bridge service that
starts alongside SteamVR, including after reboot. Display Settings controls the
desk and hand masks independently. Disable removes this installation:

```sh
python3 native-portal/run.py disable
```

For an experimental build, `enable --rollback-minutes 10` schedules automatic
uninstall; `retain` cancels that timer and keeps the persistent installation.
The rollback timer lasts only for the current boot: disable a failed trial before
rebooting. `--output` selects the build for enable; disable and retain use the
installed ownership record, so they also work after reboot without repeating it.
Never overwrite libraries mapped by the compositor.

Re-running enable can switch to a separately built output. It also migrates our
previous boot-only installation, discovering its build from the owned runtime
override and validating the exact saved ownership state before removing it. Other
preloads and Vulkan layers are preserved; our own entries are not accumulated.
Unknown or externally changed owned files are refused rather than overwritten.

Files installed outside the checkout are:

- `~/.config/systemd/user/steamvr.service.d/99-ft-passthrough.conf`
- `~/.config/systemd/user/frametop-passthrough.service` and its SteamVR wants link
- `~/.config/frametop/native-passthrough.json` (paths and ownership only)

The bridge restarts after failure and follows SteamVR stop/restart. Native
executable and shader guards remain exact. A supported update can resume the
saved intent when the desktop and native backend are ready. An unknown compositor
keeps passthrough unavailable: the bridge publishes an empty mask without current
native initialization evidence. The installation
and saved On/Off choices remain in place. Supporting a changed binary still needs
reverse engineering, a verified compatibility profile, and a separately built
output selected with enable; persistence does not make unknown updates compatible. Keep the checkout and selected build at
their installed paths. The override and bridge survive OS updates in the user
home directory; they do not modify `/opt/steamvr`.

This installs the passthrough backend, not a new desktop autostart mechanism.
Frametop's desktop still follows its launcher/session configuration. Enable and
disable restore a desktop that was running during that command; no desktop
process environment is saved for reuse after reboot.

Display Settings saves desk and hand passthrough intent independently as global
layout settings (`portal_wanted` and `hand_passthrough`). Placement and the Steam
menu preference retain their existing layout/profile behavior. Layout restore
reapplies both choices and ensures the hand tracker is running when requested.
Pause, tracking loss, a stopped desktop or an unavailable backend temporarily
remove coverage without changing those saved choices. They resume when their
required services and tracking recover. Disabling the backend with run.py removes
its installation; it does not erase the user's saved intent.

`uninstall.sh` removes persistent and legacy boot-only launch overrides, the
bridge's startup link/unit, ownership record and rollback timer without restarting
SteamVR. The loaded layer and running bridge keep working until reboot; uninstall
detects them and defers deleting the checkout.

Only the gate library is preloaded. The rendering library is an explicit Vulkan
layer; preloading its Vulkan exports would interfere with the loader chain.

## Mask boundary

The desk rectangle is the current mask producer, not the camera implementation.
`ft-screens` owns intent and placement. `bridge.py` reads a coherent
`passthrough mask` snapshot every 11 ms and publishes `mask.bin` atomically.
The layer rejects missing, invalid or older-than-500-ms data. Invalid tracking,
pause or loss of the desktop disables coverage; opening Steam menus does not.

The current 2112-byte payload begins with four float4 values:

| Value | Meaning |
|---|---|
| C.xyz | Plane center in raw tracking coordinates |
| U.xyz | Plane half-width vector |
| V.xyz | Plane half-length vector |
| control.x/y/z | Desk enabled / above overlays / hand capsule count |

The remaining 2048 bytes hold up to 64 capsules, each two float4 values:
`(A.x,A.y,A.z,ra), (B.x,B.y,B.z,rb)`. Unused slots are zero. Endpoints are in
raw tracking coordinates and radii are metres. A private 2304-byte uniform buffer
at set 1 holds these capsules, eight conservative bounding spheres and eight
capsule ranges. The layer derives the bounds and ranges from the same snapshot;
the public payload stays 2112 bytes. The native camera descriptors stay at set 0. Each native
command recording owns its buffer and descriptor until reset or destruction,
so subsequent snapshots cannot overwrite data still used by the GPU.

The other header components are reserved and zero. Orientation comes from U and V, not
from a desk-specific angle. Each native eye matrix projects camera geometry into
this plane's local coordinates. The fragment shader currently accepts the unit
rectangle. Hand coverage is the union of rays intersecting tapered capsules,
using the native eye origin separately for each eye. The shader minimizes ray
distance squared minus radius squared along each capsule and stops on the first
hit. Eight static groups reject rays outside their bounding spheres before
testing up to eight capsules per group. Rectangle hits, full-room passthrough
and empty hand snapshots skip capsule tests. The desktop uses the same bounds
to open alpha in its stereo panels. Native camera processing and the
528-byte camera uniform buffer are intact.

An isolated GLES benchmark on the Frame GPU used 50 capsules reconstructed from
a two-hand recording and the installed three-screen layout, totaling 24,652,800
stereo pixels per update. The original desktop masking pass took 65.8 ms; the
bounded version took 8.3 ms. Both versions produced identical pixel hashes for
zero, one and two hands. This benchmark substitutes a simple texture for the
desktop source and excludes native camera rendering and the rest of SteamVR.
It measures the desktop mask cost, not total compositor frame time. Verify the
complete worn-headset session after loading both graphics components, without
full-rate raw camera recording competing for storage and memory bandwidth.

The Hands option reuses upstream `handcut::Hands` capture-pose reconstruction,
motion prediction and 300-ms expiry. It does not depend on panel cutouts or the
desk toggle. The desktop publishes `hands_wanted`, `hands_valid`, and raw-space
`capsules`; invalid hands clear only hand coverage. The layer supplies fresh mask
demand to the native camera gate through call-local arguments, leaving the stock
RoomView flag untouched. Missing
or stale bridge data clears both masks. Coverage ignores capsule centres within
12 cm of the eye, matching upstream's near-eye guard. This is a hard-edged
geometric mask; upstream panel cutout feathering is not copied.

Future arbitrary masks should replace this coverage test with a supplied alpha
mask or equivalent contour representation, keeping projection separate. A planar
cockpit silhouette can use the same plane coordinates; a three-dimensional
cockpit or tracked hands/arms may need per-eye coverage produced from geometry.
Those producers should submit masks, not copy camera packets or own native GPU
lifetimes. General image/contour coverage is not implemented yet. Do not freeze
the current rectangle-and-capsules payload as the eventual external mask API.

Legacy saved desk layouts retain a hidden height offset solely to keep existing
placement unchanged. The UI and controls expose only the resulting rectangle.

## Native changes and verification

The Vulkan layer replaces only exact pinned camera shader modules, uses a private
source-over pipeline for masked draws while preserving native full-room blending,
and supplies projection push constants through compatible
pipeline layouts. Shader generation preserves native instructions and validates
all eight outputs. The separate startup gate checks the compositor hash and
instructions before modifying the Aurora gate and the existing pre/post-overlay
camera selector. [COMPATIBILITY.md](COMPATIBILITY.md) records the tested SteamOS
and SteamVR builds, every native layout/control-flow assumption and the upgrade
checks. The shared executable pin and patch constants live in `compatibility.h`.
No camera commands are copied.

Desktop status distinguishes requested intent from backend readiness and reports
an unavailable reason. Readiness combines current compositor initialization
evidence and mask publication; it does not measure
submitted camera frames. Rendering is verified on the headset.

After changes, check both eyes, On/Off, mask placement under head motion, both
Steam menu orders, grab/resize and stale-publisher shutdown. Keep targeted
geometry, UI, shader and native-interface checks. REA, GDB and frame-testbench
are optional development tools, not runtime dependencies. The previous packet,
lease and graph-observer implementation remains in Git history at `4adf5bb`.

The bridge validates native status against the compositor PID, process start time
and executable identity before publishing masks and acknowledging readiness.
Errors clear readiness and desktop alpha openings. It sends no camera On/Off
commands and has no camera-control worker. Native camera demand follows fresh
mask data, so bridge failure removes Frametop coverage without changing stock
RoomView. These checks establish initialization and publication readiness, not
rendered frames. `control.py` is an optional development tool for native camera
experiments; the installed backend does not use it.
