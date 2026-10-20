# Build from source

Most users should install the ARM64 archive described in the [README](../README.md). These instructions are for development and release packaging. Build on a Linux development machine; a host x86-64 binary cannot run on Frame.

## Dependencies

Install C and C++20 compilers, CMake 3.24+, Ninja, Python 3, pkg-config, and development headers/libraries for Cairo, Pango/PangoCairo, libxkbcommon, json-c, Wayland, SDL2, and Vulkan. The libei client is pinned and built statically from `third_party/libei`; normal builds need neither system libei nor network access. The build uses the host `wayland-scanner`. Tests also need X11 development files. Chinese/Korean release builds also need PyZy development headers/library and libhangul. Frame supplies both and the PyZy system dictionary; include their libraries in a curated sysroot. Host builds may omit these engines, but then cannot activate their composition profiles. Runtime adapters are loaded on demand from the release's `lib/framekeyboard/` directory; they are built alongside development executables in `engines/`. Optional Japanese conversion tests require Anthy and its dictionary; Japanese rendering needs a font such as Noto Sans CJK JP.

The OpenVR header is vendored, but `libopenvr_api.so` comes from an installed SteamVR runtime or a separately supplied SDK library. The build searches common Linux Steam paths. Override discovery with `-DOPENVR_LIBRARY=/absolute/path/to/libopenvr_api.so` when configuring CMake.

```sh
git clone https://github.com/TaiKeid/steam-frame-full-keyboard.git
cd steam-frame-full-keyboard
./scripts/build.sh host
ctest --test-dir build/host --output-on-failure
./build/host/framekeyboard --preview
./build/host/framekeyboard --check
```

With a custom OpenVR path, configure before running the build script:

```sh
cmake --preset host -DOPENVR_LIBRARY=/absolute/path/to/libopenvr_api.so
```

CTest reports unavailable `korean-input` and `chinese-input` suites as **Skipped**, not passed. For release validation on Frame, set `FRAMEKEYBOARD_TEST_CJK_REQUIRED=1`; unavailable engines then fail. Copy `engines/` beside transferred test executables. Run `python3 tests/engine_loading_tests.py build/host/language-tests` to check missing-module recovery in isolated copies.

The preview uses the same renderer, key state, and settings as VR but never injects OS input. `scripts/preview.sh` serves the older HTML layout study, not the application.

## ARM64 sysroot

The repository includes `cmake/steam-frame-aarch64.cmake`. It uses host Clang/LLD and target headers, GCC C++ runtime files, and libraries copied from a matching Frame system. A sysroot is not included in Git or the release archive. Recheck compatibility after Frame updates.

Set `FRAMEKEYBOARD_SYSROOT` to an absolute path outside the source checkout. It must contain:

- `usr/include`, including the target C++ standard library headers.
- `usr/lib`, with target libraries, linker names/scripts, GCC runtime files, and C runtime objects such as `Scrt1.o`, `crti.o`, and `crtn.o`.
- Both `usr/lib/pkgconfig` and `usr/share/pkgconfig`, plus library-specific headers such as `usr/lib/glib-2.0/include`.
- `opt/steamvr/bin/linuxarm64/libopenvr_api.so`.

If SSH access and rsync are configured on your Frame, this is one way to copy those installed files. Run these commands **on the development host**, replacing the SSH address with your device's address:

```sh
export FRAME_HOST=steamos@steam-frame
export FRAMEKEYBOARD_SYSROOT="$HOME/steam-frame-sysroot"
mkdir -p "$FRAMEKEYBOARD_SYSROOT/usr" "$FRAMEKEYBOARD_SYSROOT/usr/share" \
  "$FRAMEKEYBOARD_SYSROOT/opt/steamvr/bin/linuxarm64"
rsync -aL "$FRAME_HOST:/usr/include/" "$FRAMEKEYBOARD_SYSROOT/usr/include/"
rsync -aL --exclude=modules --exclude=firmware --exclude=debug \
  "$FRAME_HOST:/usr/lib/" "$FRAMEKEYBOARD_SYSROOT/usr/lib/"
rsync -aL "$FRAME_HOST:/usr/share/pkgconfig/" "$FRAMEKEYBOARD_SYSROOT/usr/share/pkgconfig/"
rsync -aL "$FRAME_HOST:/opt/steamvr/bin/linuxarm64/libopenvr_api.so" \
  "$FRAMEKEYBOARD_SYSROOT/opt/steamvr/bin/linuxarm64/"
```

Copying most of `/usr/lib` can take several gigabytes. These commands only read the device; they do not install packages or unlock SteamOS. They assume the Frame already contains the required development files. A missing header, linker name, or pkg-config file must be supplied from a matching target development environment. Do not substitute host x86-64 libraries. The initial validated sysroot was a smaller curated copy of these files; the broad copy recipe above has not been independently tested from a clean device.

## Cross-build and package

```sh
export FRAMEKEYBOARD_SYSROOT=/absolute/path/to/frame-sysroot
./scripts/build.sh frame-arm64
file build/frame-arm64/framekeyboard
./scripts/package.sh
```

The generated Wayland protocol is compiled as C. GCC and Clang builds use the same warning flags; check clean builds of both before claiming a warning-free release.

The package script builds ARM64, installs into `out/framekeyboard-VERSION-aarch64/`, includes docs/licenses/profiles, and writes an archive plus `out/SHA256SUMS`. Only the packaged executable is stripped, using the cross toolchain's `CMAKE_STRIP`; the build-directory executable retains debugging information. Packaging rejects remaining debug/static-symbol sections, a non-AArch64 binary, or embedded source/sysroot/builder-home paths. Compiler prefix maps remove build paths from diagnostics, and haptic manifests are found relative to the executable rather than through an embedded source path. Development builds copy the manifests into `build/<preset>/vr/`.

The binary retains the device OpenVR runtime path `/opt/steamvr/bin/linuxarm64`. The pinned libei client is compiled into the executable to avoid a crash in SteamOS libei 1.4.1 when negotiation is aborted. Its MIT license and provenance are included. The remaining third-party libraries come from the installed runtime.

Host and ARM64 build directories are separate. If switching sysroots or toolchains for an existing build directory, use `cmake --fresh --preset frame-arm64` before rebuilding. The build script validates the executable's AArch64 architecture.

## Tests

CTest includes `languages`, `keyboard-core`, `settings`, `placement-instance`, `japanese-input`, and `rendering`. `ei-handshake` checks peer EOF and explicit cleanup during negotiation on private socketpairs, including ARM64 builds; it never contacts a compositor or announces an input device. The rendering suite compares incremental and forced full images pixel-by-pixel across all bundled languages, controller overlaps, animation reversal, modifiers, composition, settings, and profile changes. Host builds also run installer validation/failure/retry tests. If libeis 1.6+ development files are available, `ei-recovery` tests pause/resume, disconnect, and text-socket selection using an isolated compositor and dummy Wayland listeners that cannot deliver input to the desktop. They use capture sinks and temporary configuration directories, not user applications. The placement suite uses local Unix sockets. A restricted sandbox must allow those sockets.

Anthy integration is optional in normal test runs. To require the real conversion test when the library and dictionary are installed:

```sh
FRAMEKEYBOARD_TEST_ANTHY_REQUIRED=1 ctest --test-dir build/host --output-on-failure
```

`FRAMEKEYBOARD_TEST_ANTHY_DICTIONARY` can point to a nonstandard `anthy.dic`; `LD_LIBRARY_PATH` can select a matching library. These overrides apply only to testing.

ARM64 tests must run on an ARM64 machine. For example, after cross-building:

```sh
scp build/frame-arm64/keyboard-tests build/frame-arm64/placement-instance-tests \
  build/frame-arm64/japanese-tests "$FRAME_HOST:/tmp/"
ssh "$FRAME_HOST" '/tmp/keyboard-tests && /tmp/placement-instance-tests && FRAMEKEYBOARD_TEST_ANTHY_REQUIRED=1 /tmp/japanese-tests'
```

Optional probes are excluded from CTest. `uinput-smoke-test` exclusively grabs its own device before emitting events. `ei-target-probe` and `japanese-target-probe` require focus in their own disposable Xwayland receiver. `vr-panel-probe` requires a specific input-disabled instance before posting UI events. `stick-component-probe` checks model calculations; `haptics-probe` physically vibrates controllers. Read each probe's source and usage before running it. Do not run input probes against the user's active typing session.

### Measure rendering

`render-benchmark` uses the production renderer with bundled English/Graphite profiles, a temporary config directory, and a `NullSink`. It cannot type into applications. It runs for about 35 seconds after warmup, measuring idle, one and four key presses per second, and forced redraws at a target 60 Hz. Each synthetic press lasts 100 ms; the normal 80 ms press/release animations determine redraw counts. It does not change the installed app or its settings.

After configuring the ARM64 build, run on the device:

```sh
cmake --build build/frame-arm64 --target render-benchmark
scp build/frame-arm64/render-benchmark "$FRAME_HOST:/tmp/"
ssh "$FRAME_HOST" 'XDG_RUNTIME_DIR=/run/user/$(id -u) /tmp/render-benchmark --vr'
```

Keep the headset awake and record the build type and runtime conditions. `--vr` requires SteamVR to be running already and creates its own hidden, noninteractive overlay. Omitting it measures CPU painting without connecting to SteamVR. `--full-paint` disables incremental painting, and `--rgba` forces the compatibility upload path. Combine both flags to compare with the original full-render/conversion approach. The final stress workload always forces full paints. Use an optimized build for performance comparisons; the host preset is Debug.

Output separates wall-clock painting and texture submission, with means, medians, p95 values, and process CPU time. Native upload has no pixel conversion; the compatibility path reports conversion and upload together. Upload includes staging copy, GPU fence wait, and OpenVR submission; it is not a GPU-only timer. Total redraw time includes minor measurement overhead. Before timing, the probe checks the overlay alpha flag and reads back its own texture where supported. Frame returns raw BGRA for native textures; matching those bytes verifies transfer but does not replace a headset check of colors and transparent edges. CPU percentages use one logical CPU as 100%, not the entire device.

This isolates rendering: it excludes the production controller/pose/haptic/input loop and visible compositor drawing. Idle reports the benchmark's idle cost, not the complete installed app's cost. Deadline overruns mean the benchmark loop exceeded 16.667 ms, not that SteamVR dropped a displayed frame. These measurements alone cannot establish battery life or power consumption.

### Measure replacement idle overhead

`scripts/measure-replacement.py` reads Linux process counters for the current installed keyboard, its helper and its native guard. It also reads available battery status and raw sysfs power counters. It does not send input, contact CDP, change settings or restart anything. For a Frame measurement, copy only this probe from the development host:

```sh
scp scripts/measure-replacement.py "$FRAME_HOST:/tmp/framekeyboard-measure-replacement.py"
ssh "$FRAME_HOST" 'python -B /tmp/framekeyboard-measure-replacement.py --seconds 20 --include-steam --label baseline' > baseline.json
```

Repeat with the same headset/dashboard state for a candidate build. A changed process identity invalidates the comparison. CPU percentages use one logical core as 100%. Context switches and read/write syscall counts do not identify CDP polls, and aggregate Steam webhelper CPU does not isolate `inspect()`. Short samples or charging cannot establish battery savings. Summon latency, native fallback and owner transitions need separate acceptance.

## Refresh the README image

Render the bundled default profile using an empty temporary config directory:

```sh
screenshot_config=$(mktemp -d)
mkdir -p docs/images
./build/host/framekeyboard --config-dir "$screenshot_config" \
  --render docs/images/keyboard-graphite.png
rmdir "$screenshot_config"
```

Image exports omit the interactive preview's input notice. This is a native-renderer capture, not an in-headset photograph. It contains no browser window or personal text. Inspect it before committing. Keep the README caption explicit about its source.

The `languages` suite checks all added XKB profiles and, when compiled in, real Pinyin/Hangul engines plus app commit ordering. Set `FRAMEKEYBOARD_TEST_CJK_REQUIRED=1` to make missing engines a test failure. Run this on Frame for release verification. `japanese-target-probe --multilingual` checks a fixed mixed-script phrase in its own focus-checked receiver.

`unicode-target-probe` exercises the production App with German, French, Russian, Ukrainian, Brazilian, Korean and both Chinese profiles, without a target-language declaration. It creates temporary settings, re-focuses only its own disposable Xwayland window before each press, verifies focus before delivery, and checks exact output. It restores previous focus and deletes its temporary settings. Never run it against a user field.
