# Native masked passthrough

SteamVR owns camera acquisition, stereo reprojection, GPU resources and submission.
This component changes camera coverage and ordering relative to overlays. It is
pinned to the tested Steam Frame compositor and eight native shaders; mismatched
binaries are rejected. It also affects stock RoomView while loaded.

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
services. Use the updated Display Settings to toggle passthrough and manipulate
the desk rectangle. The launch configuration lasts for this boot. Disable with:

```sh
python3 native-portal/run.py disable
```

For an experimental build, `enable --rollback-minutes 10` schedules automatic
disable; `retain` cancels that timer. Use `--output` consistently when testing a
different build directory. Never overwrite libraries mapped by the compositor.
After a reboot, disable clears saved launch state before the next enable.

`uninstall.sh` removes the owned boot-only launch override and cancels any
rollback timer without restarting SteamVR. The loaded layer and transient bridge
keep running until reboot; uninstall detects them and defers deleting the repo.

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
raw tracking coordinates and radii are metres. A private uniform buffer at set 1
holds these capsules; the native camera descriptors stay at set 0. Each native
command recording owns its buffer and descriptor until reset or destruction,
so subsequent snapshots cannot overwrite data still used by the GPU.

The other header components are reserved and zero. Orientation comes from U and V, not
from a desk-specific angle. Each native eye matrix projects camera geometry into
this plane's local coordinates. The fragment shader currently accepts the unit
rectangle. Hand coverage is the union of rays intersecting tapered capsules,
using the native eye origin separately for each eye. The shader minimizes ray
distance squared minus radius squared along each capsule and stops on the first
hit; rectangle hits skip the capsule loop. Native camera processing and the
528-byte camera uniform buffer are intact.

The Hands option reuses upstream `handcut::Hands` capture-pose reconstruction,
motion prediction and 300-ms expiry. It does not depend on panel cutouts or the
desk toggle. The desktop publishes `hands_wanted`, `hands_valid`, and raw-space
`capsules`; invalid hands clear only hand coverage. The bridge keeps native
RoomView requested while either desk coverage or hand intent needs it. Missing
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

The Vulkan layer replaces only exact pinned camera shader modules, sets camera
source-over blending, and supplies projection push constants through compatible
pipeline layouts. Shader generation preserves native instructions and validates
all eight outputs. The separate startup gate checks the compositor hash and
instructions before modifying the Aurora gate and the existing pre/post-overlay
camera selector. [COMPATIBILITY.md](COMPATIBILITY.md) records the tested SteamOS
and SteamVR builds, every native layout/control-flow assumption and the upgrade
checks. The shared executable pin and patch constants live in `compatibility.h`.
No camera commands are copied.

The desktop status reports only `wanted` and `connected`; it does not pretend to
measure submitted frames. Rendering is verified on the headset.

After changes, check both eyes, On/Off, mask placement under head motion, both
Steam menu orders, grab/resize and stale-publisher shutdown. Keep targeted
geometry, UI, shader and native-interface checks. REA, GDB and frame-testbench
are optional development tools, not runtime dependencies. The previous packet,
lease and graph-observer implementation remains in Git history at `4adf5bb`.

The bridge acknowledges its heartbeat only after publishing a valid mask and
delivering the RoomView request. Errors clear readiness, so desktop alpha does
not stay cut out when publication fails. This is transport readiness, not
confirmation of rendered camera frames. If native RoomView resets independently
while intent remains unchanged, toggle passthrough Off/On to reassert it.
