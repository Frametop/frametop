# Input proxy protocol and build

## Build and offline tests

```sh
bash scripts/build-input.sh                # build/libframe_input.so + libframe_loader.so
bash scripts/build-input.sh /private/out   # optional isolated output directory
python3 -m unittest discover -s tests -v
CXXFLAGS='-fsanitize=undefined -fno-sanitize-recover=all' python3 -m unittest discover -s tests -p test_input_proxy.py -v
```

Requires Linux, a C++17 g++ toolchain, libc/libdl/pthreads, and Python 3 for tests.
The script honors CXX and CXXFLAGS. Build natively for the target architecture.
It statically links the C++ runtime and hides its symbols, avoiding SteamVR's
potentially different libstdc++. Public exports are only HmdDriverFactory for
the proxy, and dlopen/frame_real_dlopen for the loader.

The tests build a fake cv DSO and an executable named vrserver. They dynamically
load the production proxy through the production loader, exercise every public
host/input forwarding method, preserve component identity, and make real UNIX
socket requests. They do not load SteamVR, access a headset, or restart services.
Test artifacts go under build/input-tests. These are CPU/ABI tests, not evidence
of live Frame tracking, compositor rendering, or XRService presence behavior.

## Loader environment

Only the vrserver executable, identified by the basename of /proc/self/exe,
redirects dlopen. Other processes inheriting LD_PRELOAD are unchanged.

* LD_PRELOAD: absolute path to libframe_loader.so.
* FRAME_TESTBENCH_PROXY: absolute path to libframe_input.so, required to redirect.
* FRAME_TESTBENCH_SOCKET: optional absolute UNIX socket path. Default:
  /run/user/<uid>/frame-testbench.sock.
* FRAME_TESTBENCH_REAL_DRIVER: optional exact absolute cv module path. Default:
  /opt/steamvr/drivers/cv/bin/linuxarm64/driver_cv.so. Tests set this to the fake
  module. Do not point it at the proxy.

The loader matches only that exact absolute path, not substrings, relative paths,
other modules, dlopen(NULL), or alternate spellings. The proxy loads cv through
frame_real_dlopen, which calls the un-interposed RTLD_NEXT dlopen. This keeps the
real cv module at its original path and preserves its $ORIGIN dependencies. No
/opt modifications, vtable patches, or settings writes are performed. The proxy
registers two additional synthetic controller devices through the public host ABI.
Provider004, host006, and input004 use the pinned public Valve header.
All other requested interface versions pass through unchanged.

Installation, service environment, initial restart, live acceptance, and recovery
are the caller's responsibility. A missing proxy/cv DSO or a control socket that
cannot be created causes cv initialization to fail rather than pretending control
is available. Removing the service environment and restarting restores ordinary
loading. The source makes no service changes itself.

## Socket security and lifetime

The socket is chmod 0600 before listen. The parent directory must belong to the
runtime UID and must not be group/other writable. SO_PEERCRED restricts clients
to the runtime UID. Live sockets, symlinks, and ordinary files are never replaced.
An owned stale socket is reclaimed only after ECONNREFUSED and an inode check.
Cleanup removes only the same inode created by this instance.

Each connection sends one ASCII text command followed by newline and receives one
JSON line, then closes. Requests are bounded to 1024 bytes. A partial request has
a two-second connection deadline. This is only a transport timeout: overrides
have no lease or disconnect timeout. They remain until release or provider restart.
Up to 32 concurrent clients are serviced without blocking the nominal 90Hz tick.

## Commands

```text
status
pose x y z qw qx qy qz
controller-pose left|right x y z qw qx qy qz
controller-release left|right|all
controller-inputs left|right /input/path value [/input/path value ...]
controller-input-release left|right|all
worn 0
worn 1
release
pose-release
worn-release
```

Pose coordinates are RAW tracking coordinates in meters. Quaternion order is
w,x,y,z. Every value must be finite, and the quaternion must be nonzero. The proxy
normalizes it with overflow-safe scaling. Synthetic poses have identity
world-from-driver and driver-from-head rotations, zero translations for those
transforms, zero velocity/acceleration, poseTimeOffset=0, connected/valid=true,
and TrackingResult_Running_OK. This is a stationary pose, not a trajectory.

Only the activated cv HMD index and /proximity components belonging to its exact
property container are intercepted. Every matching proximity handle is supported.
Physical controllers and other components are forwarded unchanged. Real HMD pose and
proximity updates are cached but suppressed independently while their respective
override is enabled. Commands publish immediately; the worker republishes at a
nominal 90Hz even without physical updates or provider RunFrame calls.

release clears HMD pose, worn, and both synthetic controller overrides.
pose-release and worn-release clear only their named HMD channel.
controller-release neutralizes inputs and disconnects one hand or both with all,
without releasing HMD channels. controller-input-release neutralizes inputs only,
keeping controller poses connected. A failed neutralization returns ok:false and
keeps that hand connected so the caller can retry. Other hands and HMD channels
still release when global release encounters such a failure.
Releasing an HMD channel immediately forwards its latest cached physical sample,
including original driver transforms and validity. If no physical sample has arrived yet,
that channel resumes forwarding at the next physical update. No physical state is
invented. Cached samples can be stale when cv is in standby.

HMD Deactivate clears its readiness and stops HMD publication before forwarding
the real Deactivate. Cleanup joins the worker, disconnects synthetic controllers,
clears their readiness, then calls real provider Cleanup.

## Synthetic controllers

The provider registers frame_testbench_left and frame_testbench_right as separate
TrackedDeviceClass_Controller devices. Each has its explicit left/right role hint
and the built-in generic_controller render model. They advertise controller type
frame_controller, model Steam Frame Controller, manufacturer Valve and input profile
{frame_controller}/input/frame_controller_profile.json. That resource must already
exist in the installed runtime. This repository does not distribute Valve's private
profile, bindings, icons or models. The generic render model remains until exact
side-specific resource names and rendering can be verified on a live runtime.
These devices never wrap a physical controller or depend on one waking up.
Action bindings and application-specific rendering need separate live verification.
Skeleton and haptic components are not registered or emulated.

Each hand registers 25 input components, 21 booleans and 4 absolute normalized scalars:

| Side | Paths | Type/range |
| --- | --- | --- |
| Both | /input/system/click, /input/system/touch | boolean |
| Both | /input/bumper/click, /input/bumper/touch | boolean |
| Both | /input/trigger/click, /input/trigger/touch | boolean |
| Both | /input/grip/click, /input/grip/touch | boolean |
| Both | /input/thumbstick/click, /input/thumbstick/touch | boolean |
| Both | /input/thumbrest/touch | boolean, no thumbrest click |
| Both | /input/trigger/value, /input/grip/value | scalar [0,1], NormalizedOneSided |
| Both | /input/thumbstick/x, /input/thumbstick/y | scalar [-1,1], NormalizedTwoSided |
| Left | /input/view/click, /input/view/touch | boolean |
| Left | /input/dpad_up/click, /input/dpad_up/touch | boolean |
| Left | /input/dpad_right/click, /input/dpad_right/touch | boolean |
| Left | /input/dpad_down/click, /input/dpad_down/touch | boolean |
| Left | /input/dpad_left/click, /input/dpad_left/touch | boolean |
| Right | /input/menu/click, /input/menu/touch | boolean |
| Right | /input/a/click, /input/a/touch | boolean |
| Right | /input/b/click, /input/b/touch | boolean |
| Right | /input/x/click, /input/x/touch | boolean |
| Right | /input/y/click, /input/y/touch | boolean |

controller-inputs requires successful input registration and initialization plus
an active controller-pose override for that side. At least one path/value pair is
required. Boolean tokens must be exactly 0 or 1. Scalars must be finite and in
range before conversion to float. Unknown or wrong-side paths, duplicate paths,
extra tokens, missing values and invalid values reject the entire batch without
changing state, sequence or any runtime component. Touch/click/value channels are
independent; trigger value does not synthesize a click or touch.

Validation is atomic, but OpenVR writes are not transactional. After validation,
components publish in caller order at time offset zero. A rejected SDK write stops
the batch, returns ok:false and records input_error with the rejected path and SDK
code. Earlier successful writes remain applied, later writes are not attempted,
and sequence advances. There is no rollback claim. inputs contains only the last
successfully published values, using JSON booleans and the actual SDK float values.
Read status, then retry or use controller-input-release to recover. SDK acceptance
is not an application action-state readback. Inputs stay latched until changed or
released, without synthetic periodic re-publication.

Release attempts every registered component even if some writes fail. Successful
neutralizations update inputs; failures retain the last confirmed value. Explicit
release returns ok:false on any rejected write. Runtime Deactivate cannot be
refused: it attempts neutralization before disconnecting, logs SDK failures, and
invalidates all handles regardless. A deactivated side retains its last error in
status but has inputs:{} and inputs_ready:false. Cleanup attempts release before
deactivation and logs any remaining forced-release failures. No calls use old
handles after deactivation; reactivation registers a fresh set.

Registration failures leave controller pose control available but inputs_ready
false. Only successfully initialized components appear in inputs. Reactivation
is required to retry failed registration or initialization.

Registration runs outside the state lock because the runtime can synchronously
call Activate and GetPose. The devices live for the provider's lifetime. Failed
registration or missing activation leaves device_index null; controller-pose
then returns ok:false with error:"controller is not active". HMD commands remain
available if only controller registration fails.

Activate publishes a disconnected, invalid pose with identity rotations, zero
translations and velocities, and TrackingResult_Uninitialized. A controller-pose
command connects that hand with a valid pose using the same RAW coordinate and
quaternion rules as the HMD pose command. Each hand persists independently and
publishes immediately and at the nominal 90Hz tick, even when the physical HMD
is invalid, inactive, or not producing updates. GetPose returns the selected pose.

controller-release first neutralizes the selected inputs. On success it clears
the selected pose request and immediately publishes the explicit disconnected pose. It then stops ticking that hand; status pose becomes
null while device_index remains assigned. Release of an inactive or already
released hand is accepted. Runtime Deactivate clears that hand's request and
index and stops publication, without changing the other hand or HMD. Unlike HMD
overrides, controller requests do not survive Deactivate. Provider Cleanup also
rejects subsequent controller activation until another Init.

The state lock serializes publication, release and deactivation. It permits
same-thread runtime callbacks such as GetPose during TrackedDevicePoseUpdated;
registration and property writes do not hold that lock.

## Response fields

Every response, including rejected commands, contains:

* ok: boolean, true when a command was accepted or status was read. This is not a
  client-side or compositor readback guarantee.
* sequence: monotonically increasing override/capture mutation counter, reset by
  provider Init. Status reads and malformed requests do not change it. Physical
  samples and periodic republications do not change it.
* pose_override: boolean.
* worn_override: null when released, otherwise the requested boolean.
* pose: selected DriverPose data, either requested override or latest physical;
  null if neither exists. Contains position:[x,y,z], quaternion:[w,x,y,z], valid,
  connected, world_from_driver, and driver_from_head.
* physical_pose: latest unmodified real DriverPose in the same JSON shape or null.
  Its position/quaternion are driver-space values, not a silently assumed raw or
  Standing pose. The two transform objects preserve the conversion inputs.
* requested_pose: null or [x,y,z,qw,qx,qy,qz], normalized.
* physical_worn: null if unknown, otherwise the latest physical value for the first
  matching proximity handle.
* hmd_index: captured Activate index, or null when inactive.
* hmd_container: captured property container, zero when inactive.
* proximity_ready: boolean, true when a matching /proximity handle is captured.
* proximity_handles: all captured matching handles.
* controllers: object with left and right keys. Each value contains:
  * device_index: assigned synthetic index, or null before activation/after deactivation.
  * pose_override: boolean, true when this hand has a requested pose.
  * pose: requested pose in the same shape as the HMD pose field, or null when released.
  * synthetic: always true. These are not captured physical controllers.
  * inputs_ready: true after every component registers and its initial neutral
    update succeeds. False before activation, after deactivation or on partial
    registration/initialization failure. Later update errors do not clear readiness.
  * inputs: exact component path to boolean/number map of confirmed SDK writes.
    Empty before activation/after deactivation. Missing paths indicate components
    without a successful initialization, never invented neutral values.
  * input_error: null or {path:string, code:integer}, the last registration,
    initialization or publication failure. A successful input batch or release on
    a ready controller clears this error; malformed requests and status do not.
* error: fixed explanatory string on rejected requests.

Pose and worn commands reject missing HMD/proximity readiness. On Deactivate the
requested override fields persist but hmd_index is null and publication stops.
The header ABI and offline tests do not establish that low-level sensor presence,
headset suspend, compositor reprojection, or application poses respond correctly
on a particular SteamVR build. Verify those separately before claiming live use.
