# Frametop feature map (step 1: features, links, proposed chart groups)

Source: `~/frametop/.worktrees/experimental`, committed tree **`403c1eb`** (branch `experimental`, 2026-10-30), read with `git show` / `git grep`. Not reflected: the uncommitted H1/R1/H2 work in that checkout (`screens/remote.c`, `stream/ft-stream.cpp`, `stream/hostinput.h`, `stream/test/hostinput-test.cpp` modified), and the untracked `.claude/skills/verify-remote-streams/` (copied read-only to `snapshot/` here; it is where H1, H2, R1, the slot leak and the doc drift are written up). Nothing was run against live processes or hosts.

Overview chart: `feature-overview.mmd` / `.svg` / `.png` / `-small.png` in this folder.

Conventions. `@name` = abstract unix datagram socket. `RUN` = `/run/user/UID` (the session's `XDG_RUNTIME_DIR`). "box" = the `dev` Fedora container, entered through `scripts/in-box`. Config files are under `~/.config/` unless a path says otherwise.

---

## 1. Features

### F1. SteamVR start, units and the desktop session
- **Does:** starts Frametop's services with SteamVR; starts and stops the nested Plasma desktop ("Desktop" in Launch a program); keeps apps alive across desktop restarts.
- **Processes:** `desktops.sh`, `session/frametop-session.sh` (host bash), `session/autostart.sh`, `session/keep-apps.sh`, `session/fix-panels.py`; nested `kwin_wayland` (via a generated `kwin_wayland_wrapper` with screens + float slots outputs) and Plasma on the host; Plasma autostart children `ft-atspi`, `ft-floatd`, `ft-dashroute`, `ft-notifyfwd`, `ft-tour --first-run`; D-Bus-activated `ft-gtksettings`.
- **Files:** `desktops.sh`, `session/*`, `decoration/` (KWin decoration with the float button), `session/frametop.conf.example` (every conf key).
- **Config:** `BACKEND` (screens|gamescope), `AUTOSTART`, `NOTIFY_FORWARD`, `FIREFOX_TITLEBAR`, `FLOAT_SLOTS`, `REMOTE`; `~/.config/frametop/` (the desktop's own KWin/Plasma config, `frametoprc` one-time markers, `autostart/` Hidden copies).
- **Units:** `frametop-autostart.service` (oneshot, PartOf steamvr), transient `frametop-desktop` (systemd-run from `desktops.sh start`), `steamvr.service` (Valve's; most Frametop units are `PartOf`/`WantedBy` it).
- **Logs:** `RUN/frametop-layout.log`, `~/.local/state/frametop/screens.log`, `/tmp/frametop-*.log`, `journalctl --user -u frametop-autostart`.

### F2. ft-screens: the compositor and its VR panels
- **Does:** wlroots compositor hosting nested KWin; each KWin output = one SteamVR overlay `frametop.screen.N` (DMA-BUF import, no copy). Panel controls (bar, curve, roll, resize, reset, pin), visibility modes, wrist/head pins, per-screen frame rates, lazy-susan `spin`, `pause on|off`, VR keyboard fallback panel, held-keys release (`screens/held-keys.h`), Gamepad pill (`frametop.gamepad` hidden dashboard overlay), universe fix (`screens/universefix.h`), hosts remote-display panels (F14) and hand near-input (F11).
- **Processes:** `ft-screens` (box; `screens/compositor.c`, `screens/vr.cpp`, `screens/remote.c`, hand files).
- **IPC:** owns `@ft_screens` (text datagrams, reply to sender; full verb list in `docs/reference.md`); Wayland socket `ft-screens-0` for KWin. Called by ft-layout, ft-floatd, frunner, relay, pointer helper, dashpatch, ft-gaze, ft-handsctl, Display/Input/Remote settings apps, tour, VNC bridge, reports.
- **Config:** `~/.config/frametop-layout.json`; conf keys `SCREEN_*`, `HANDS_*` (near input), visibility/controllers settings; `ft-screens --rates`.
- **Units:** none of its own; runs inside the `frametop-desktop` unit's session (container scope via `in-box`).

### F3. Layout, profiles and Display Settings
- **Does:** arranges screens around the head, saves/opens profiles (positions, pins, hidden screens, apps), quick reset with countdown, Plasma output arrangement following VR positions, per-screen scale to KWin.
- **Processes:** `layout/ft-layout` (`ft_layout.py`, host python, stdlib only), `layout/countdown/ft-reset-timer`, `ft-layout-reset`, `ft-hide-show`; Frametop Display Settings (`display-settings/ft_display_settings.py` + `main.qml`, Kirigami in box) with `steam_settings.py` (Steam sleep setting over CEF 8080).
- **Files/state:** `~/.config/frametop-layout.json`, `~/.local/state/frametop/layout-anchor.json`, `docs/profiles.md`.
- **IPC:** `@ft_screens` (`place`, `pin`, `scale`, `conceal`, `screens`, `head`, `universe`...), `@ft_powerd` (Power tab), `@frametop_float` (profile apps).

### F4. Floating windows, frunner and commands
- **Does:** a KWin window floats as its own panel on a spare output; Launch as Standalone; dock/tear-off/density; SteamVR-dashboard launches routed into the desktop (`DASHBOARD_APPS=frametop`). frunner = Meta+Space search pill (apps, profiles, remote displays, commands). Commands = named shell commands run as transient units.
- **Processes:** `float/ft-floatd` (`ft_floatd.py`, in Plasma session) + KWin script `float/frametop-float.js`; `float/ft-float` CLI; `float/ft-dashroute` (wraps `SteamClient.Apps.LaunchNonSteamApp` over Steam CEF 127.0.0.1:8080, 3 s heartbeat); `frunner/ft-frunner` + `ft-spotpanel` (box); `commands/ft-cmd`.
- **IPC:** owns `@frametop_float`, `@ft_frunner`, `@ft_spotpanel`; uses `@ft_screens` (`float`, `pose`, `sub`, `hold`, `carry`, `torn`, `spin`, `front`, `frunner on|off`). Commands run as `frametop-cmd-NAME-TIME` transient units.
- **Config:** `FLOAT_SLOTS`, `FLOAT_MARGIN`, `FLOAT_DOCK_DISTANCE`, `DASHBOARD_APPS`, `SCREEN_SPIN_*`; `~/.config/frametop-float.json`, `~/.config/frametop-frunner.json`, `~/.config/frametop-commands.json`, `~/.config/frametop/aliases.sh`.

### F5. Input relay (mouse, keyboard, bindings, typing target)
- **Does:** stable uinput virtual mouse/keyboard/gamepad for SteamVR; grabs USB/BT devices by role (3D pointer, pass-through, keyboard+pointer, ignore); key bindings and taps (Meta tap = Steam menu, Meta+J/K gaze clicks, Meta+Shift+F/H, Ctrl+Alt+Shift+Esc release keys...); owns volume keys; decides where typing goes (desktop vs Steam); network devices for host input (F15); types voice and Full Keyboard text.
- **Processes:** `input/input-relay.py` (host python, `Type=notify`, fd store keeps virtual devices across restarts), `input/ft-textinput` (KWin input method: `textfield 1|0`), `input/ft-clickctl`, `input/rules-migrate.py`; Frametop Input Settings (`input-settings/`, box).
- **IPC:** owns `@frametop_relay`; sends to `@ft_screens` (keys, `vrkeyboard`, `pause`), `@ft_pointer_helper` (motion, `vrbind`, `calkey`, `typing`), `@ft_gazed` (quickcal), `@frametop_float`, `@ft_frunner`, `@frametop_voice`; runs `steam/ft-steam menu` (Steam CEF 8080) and `command:` bindings with `sh -c`.
- **Config:** `~/.config/frametop-input.json` (roles, `key_bindings`, `controller_buttons`, `vr_keyboard`, `pause_*`), conf `POINTER=1`; `~/.local/state/frametop/input/rules-migrations.json`.
- **Units:** `frametop-input-relay.service` (Before=steamvr, WantedBy default + steamvr).

### F6. 3D pointer (helper, driver, dashboard patch)
- **Does:** a mouse/hand/gaze-driven laser as a SteamVR treadmill-role virtual controller; overlay hit tests, dot, head follow, controller buttons via SteamVR input (sends `vrbtn` to relay), says when a VR game runs (`vrgame 1|0`), hand ray/pinch clicks (F11), gaze mode clicks and lessons (F10), desk classifier, typing posture. Dashboard patch lets the treadmill move SteamVR windows and reports Gamepad mode / theater app.
- **Processes:** `ft-pointer` (box, `pointer/helper/*.h`), `driver_ft_pointer.so` (inside vrserver), `pointer/dashpatch/ft-dashpatch.py` (SteamVR web helper debugger 127.0.0.1:8089), probes.
- **IPC:** owns `@ft_pointer_helper`; driver owns `@ft_pointer` (pose); dashpatch owns `@ft_dashpatch` (`gamepad` from ft-screens) and sends `steamvrmode` to `@ft_screens`; helper sends `hands L R allowed` to `@ft_screens`, `gz`/lessons with `@ft_gazed`.
- **Config:** `POINTER_*` (~80 keys), `driver_ft_pointer` in steamvr.vrsettings, `VRWebHelper/DebuggerEnabled`.
- **Units:** `frametop-pointer.service`, `frametop-dashpatch.service` (both PartOf steamvr).

### F7. Worn / headset presence
- **Does:** one answer to "is someone wearing it": HMD activity level Idle/Standby/Idle_Timeout for 1.5 s (`HEADSET_OFF_AFTER`) or Standby at once = away.
- **Owner:** `ft-pointer` (`pointer/helper/ft-pointer.cpp` ~l.955-1970), answered on `@ft_pointer_helper` as `gaze ? headset` -> `ok on|off worn|away`.
- **Consumers:** ft-gazed (idle when away), ft-stream (host-input lease, F15), the helper itself (pointer lets go; hands `allowed` to ft-screens), quick check on headset-on (ft-gazed via eye log). **Separate readers:** `ft-powerd` reads the activity level itself (no 1.5 s rule); the testbench fakes HMD proximity inside vrserver (`worn on|off|physical`), which every reader above sees as real.
- **Test-only state:** `RUN/frametop-fake-worn.json`, `RUN/verify-frametop.json`, `RUN/frametop-input-HOST.lock` (held by the guard).

### F8. Pause for VR games and Gamepad mode
- **Does:** pause hands the CPU/GPU to a VR game: stops `frametop-gaze`, stops or parks hands, hides (or closes) the desktop, stops VNC, releases the 3D mouse, routes typing to Steam. Auto on scene apps (`vrgame` from helper, 5 s resume after the game ends), controller gesture (both sticks twice, read from vrserver's web socket), mapped action, `ft-pause`. Gamepad mode: Frame controllers as a virtual Xbox pad to a desktop panel or a remote display.
- **Processes/files:** in the relay: `input/game_pause.py`, `input/vrws.py`, `input/vrpad.py`, `input/ft-pause`.
- **State:** `RUN/frametop-pause.json` (survives relay restart), `RUN/frametop-hands/park`.
- **Config:** `pause_auto`, `pause_gesture`, `pause_desktop`, `pause_sound` in `frametop-input.json`.
- **IPC:** `systemctl --user stop/start` on gaze and hands units, `@ft_screens pause on|off`, `session/remote-ctl.sh stop`, `desktops.sh stop|start`; answers `pause ?` (ft-stream asks every second).

### F9. Display power
- **Does:** backlight off after `DISPLAY_OFF_MIN` unused (moves > 5 mm / 0.5 deg, any input event), on at next use; waits while SteamVR standby owns the displays. Stay awake while plugged in (Steam setting via CEF).
- **Processes:** `ft-powerd` (box). **IPC:** owns `@ft_powerd`; writes `/sys/class/backlight/ae94000.dsi.0/brightness`; `~/.cache/frametop/powerd-brightness`.
- **Config:** `DISPLAY_OFF_MIN`, `DISPLAY_MOVE_MM`, `DISPLAY_MOVE_DEG`, `STEAM_SLEEP_AC_BEFORE`. **Unit:** `frametop-power.service`.

### F10. Gaze and eye tracking
- **Does:** gaze mode for the 3D mouse (MAGIC pointing), calibration/quick check/fit check panels, nudge learning, one-eye handling, idle when unused, optional fusion; our own clean-room eye tracker as an alternative to SteamVR's.
- **Processes:** `gaze/ft-gazed` (host python service) runs `ft-gaze` (C++, box; stdin/stdout JSON lines at 90 Hz; reads `/dev/shm/eye-server.mmap` read-only and the SteamVR eye action) and `ft-gazepanel` (head-locked panel, `@ft_gazepanel`, driven by `gaze/gazecheck.py`); own tracker: `ft-eyegrab` (root, `/etc/frametop/ft-eyegrab`, copies eye-camera frames to `/dev/shm/frametop-eyes-cams` while `/dev/shm/frametop-eyes-want` is fresh) and `ft-eyes` (python, owns `@ft_eyes`, writes `/dev/shm/frametop-eyes-gaze`); tools `ft-gazectl`, `gaze/eye-rate.py`, probe, bench.
- **IPC:** owns `@ft_gazed` (wake lease from Input Settings, quickcal from relay, lessons/recheck from helper, status); sends `gz ...` to `@ft_pointer_helper` 90/s; asks `gaze ? headset`; `@ft_screens` for pixel mapping.
- **State:** `~/.local/state/frametop/gaze/` (calibration, `fusion.json`, `checks.jsonl`, `pointer-lessons.jsonl`).
- **Config:** `POINTER_GAZE*`, `GAZE_TRACKER`, `GAZE_EYE`, `GAZE_FIT_WATCH`, `GAZE_FUSION`, `EYES_SIZE_GUARD`; SteamVR `steamvr.gazeDepEyeToHeadEnable`.
- **Units:** `frametop-gaze.service` (PartOf steamvr), `frametop-eyegrab.service` (system, root, multi-user).

### F11. Hand tracking, cutouts, near input and rendered hands
- **Does:** camera broker -> tracker -> hands and gestures; hand cutouts (room view through screens), near input (poke, pinch, card/fist grabs, throws, "grab that", palm menu), hand ray and pinch clicks in the pointer, gaze+pinch, rendered 3D hands, flags (last 12 s for offline tools), auto side-swap detection, park during games.
- **Processes:** `ft-camd` (host, file caps via sudo, borrows XRService camera buffers) -> `RUN/frametop-hands/cam-ring`; `ft-hands` (box, ncnn models) -> `RUN/frametop-hands/{hands,gestures,sides.json,events.jsonl}`; reads `park`, `want-hz` (from ft-screens); `ft-handview` (box); `hands/ft-handsctl`, `hands/handsmode.py`, `hands/ft-camwatch`; ft-screens `handinput/handcut/palmmenu/grabthat`; helper `handsin.h`, `gazehand.h`, `typing.h`, `deskclass.h`.
- **Config:** `HANDS_MODE` (off|frametop|always), `HANDS_*` (~70 keys), `POINTER_HANDS`, `POINTER_RAY_*`, `POINTER_PINCH_*`; models pinned by `hands/models.pin` / `models.json`.
- **Units:** `frametop-camd.service`, `frametop-hands.service`, `frametop-handview.service` (PartOf steamvr), `frametop-camwatch.service` (default.target).

### F12. Keyboards and text input
- **Does:** Full Keyboard for Steam Frame (TaiKeid, vendored MIT, `keyboard/`) replaces Steam's keyboard, touch typing, layouts/IMEs; ft-screens' own key panel is the fallback; opens on text-field focus.
- **Processes:** `framekeyboard --frametop` (box); `input/ft-textinput` (KWin IM).
- **IPC:** framekeyboard <-> `@frametop_relay` (`keyboard/include/framekeyboard/frametop.hpp`: typing and `vrkeyboard` show/hide relayed from ft-screens); `textfield` -> relay -> `@ft_screens vrkeyboard`.
- **Config:** `vr_keyboard`, `vr_keyboard_persist` (frametop-input.json), `~/.config/frametop-keyboard`, `~/.config/frametop/kxkbrc` (desktop layout for typing). **Unit:** `frametop-keyboard.service`.

### F13. Voice dictation and wake words
- **Does:** on-device dictation (Moonshine on CPU; whisper.cpp on GPU for wake words/other languages), types where typing goes; voice commands ("run NAME", "flag that").
- **Processes:** `voice/ft-voiced` (box venv), `whisper-server` via `voice/whisper-run`, `voice/ft-voice` CLI, `voice/hooks/on-command`.
- **IPC:** owns `@frametop_voice` (`toggle|start|stop|cancel <source>` from the relay); sends `type TEXT` to `@frametop_relay`; reads controller buttons from vrserver's web socket; `pw-record` mic.
- **Config:** `~/.config/frametop-voice.json`; models in `~/.local/share/frametop/voice/models` (`voice/models.sh`, sha256-pinned).
- **Units:** `frametop-voice.service` (default.target), `frametop-voice-whisper.service` (PartOf voice, on demand).

### F14. Remote displays and streaming
- **Does:** other computers' monitors (or virtual ones) as Frametop panels via Moonlight protocol from Vibepollo (Frametop fork) or Sunshine (Mac, one instance per display). Pairing with a narrow API token and pinned cert, per-display client cert, dongle/network route, host audio (one stream per host), Frame mic to host, gamepad pad to host, panels with screen controls, profiles keep positions.
- **Processes:** Frametop Remote Displays app (`remote-displays/`, box); `ft-layout remote ...`; `screens/remote.c` inside ft-screens (stream slots, restart backoff 2-30 s, `host_busy`); one `ft-stream stream` per display (box; `stream/ft-stream.cpp`, `host.cpp`, iris HW decoder, moonlight-common-c with `moonlight-hostin.patch`, `moonlight-mic.patch`); host kits `host/windows`, `host/macos` (`ft-sunshine`, Frametop Streams menu), `host/linux`; the Vibepollo fork lives in another repo (`Frametop/frametop-vibepollo`).
- **IPC:** remote.c forks ft-stream with a SOCK_SEQPACKET pair (text messages + 3 dmabuf fds via SCM_RIGHTS; `state`, `buffers`, `frame`, `release`, `attention`, `move/button/scroll/key/pad/blur/quit`); stop = SIGTERM, reap in `ft_remote_child`; ft-stream -> host: HTTPS pairing/launch, RTSP/ENet stream, control channel; `flock` on `RUN/frametop-audio-HOST.lock`.
- **Files:** `~/.local/share/frametop-stream/NAME` (keys), `.../hosts/HOST.token`; layout entries in `frametop-layout.json` (`"input"`, `"mic"`, route).
- **Units:** none (children of ft-screens).

### F15. Host input lease (a computer's keyboard and mouse in Frametop)
- **Does:** while worn, live, switch on, not paused, not escaped and no test forbids it, the stream holding the host's input lock sends `engage SEQ 1500` every 500 ms; host's events come back and become relay "network devices".
- **Code:** `stream/ft-stream.cpp` `struct HostInput` (`Open`, `Tick`, `TakeAnswers`, `Apply`, `Stop`, `Unlock`), `stream/hostinput.h` (`Decide`, `Fresh`, `AfterTest`, `TestForbidding`, `Relay`), `input/input-relay.py` network devices (`hostdev`, `hostbye`, 3 s hello timeout), host side `vibepollo-host-input` (Linux, socket-activated; other repo).
- **IPC/state:** `flock RUN/frametop-input-HOST.lock`; asks `@ft_pointer_helper` `gaze ? headset` and `@frametop_relay` `pause ?` every 1 s; host datagrams to `@frametop_relay`; protocol `0x3005` lease (Frame->host), `0x3004` events/state/hello (host->Frame); reads `RUN/frametop-fake-worn.json`, `RUN/verify-frametop.json` every tick.
- **Config:** layout `"input": true` per host (`ft-layout remote host NAME input=on|off`).

### F16. Remote desktop over VNC
- **Does:** serves the primary screen over VNC on the Tailscale address (5900).
- **Processes:** `session/remote-desktop.sh` (`krdpserver --plasma` on 127.0.0.1:3390), `session/vnc-bridge.sh` (Xvnc `:20` + FreeRDP on demand), `session/remote-ctl.sh`, Frametop Remote Access (`remote/ft-remote-settings`).
- **IPC:** `@ft_screens watch 15` every 5 s while a viewer is connected; `ft-layout remote-view`. Nested KWin runs with permission checks off when on.
- **Config:** `REMOTE=1`, `~/.config/frametop-remote/vnc-password`, `VNC_IDLE_SEC`.

### F17. Steam and SteamVR UI bridges (cross-cutting)
- `steam/ft-steam` + `steamui.py` (Steam menu over CEF 127.0.0.1:8080), `display-settings/steam_settings.py` (sleep setting, same port), `float/ft-dashroute` + `dash-route.js` (same port), `pointer/dashpatch` (SteamVR web helper debugger 127.0.0.1:8089), `input/vrws.py` (vrserver web socket: controller buttons for pause gesture, voice, gamepad). All depend on undocumented Steam/SteamVR UI; `scripts/update-check.py` guards several.

### F18. Config and settings apps (cross-cutting)
- Files: `~/.config/frametop.conf` (backend, pointer, gaze, hands, power, float, notify...), `frametop-layout.json` (screens, profiles, remote displays and host switches), `frametop-input.json` (devices, bindings, controller buttons, pause, keyboard), `frametop-voice.json`, `frametop-float.json`, `frametop-frunner.json`, `frametop-commands.json`, `frametop-keyboard/`, `~/.config/frametop/` (nested Plasma).
- Apps: Frametop Display Settings (F3), Frametop Input Settings (F5, talks to relay, helper, gazed, screens), Frametop Remote Displays (F14), Frametop Remote Access (F16), Frametop Tour (`tour/ft-tour`, opens pages via `FT_INPUT_PAGE`/`FT_DISPLAY_PAGE`, tries frunner/spin/gaze cal).
- Migration: `scripts/conf-migrate.sh`, `input/rules-migrate.py`.
- Who re-reads live: helper and relay on settings-app messages; ft-gazed on file change (four gaze keys); voice within 2 s; most others at start.

### F19. Build, install, update and release
- **Dev/build:** `dev` container (`setup/dev-container.sh`, `scripts/container-up.sh`, `scripts/in-box`, `scripts/frame.sh`, `scripts/sync.sh`, `scripts/openvr.sh`), per-component `build.sh`, `./ft dev build|test`, `justfile`.
- **Install:** `install.sh` (offers each part), `get.sh` (release download / `--zip`), `uninstall.sh` (owns everything installers write, incl. caps and `/etc`), `desktops.sh install|relay install|autostart|remote`, per-part `run.sh install`.
- **Release:** `pack/Containerfile` (OCI image, pinned), `pack/install-release.sh`, `pack/release-box.sh`, `framedrop/` (Frametop.zip, FrameDrop installer unit), `.github/workflows/release.yml` (Depot arm, `frametop-*` tags), `frametop-release.json`.
- **Live patching:** `scripts/fast-install` (commits into the live release, restarts services; `~/.cache/frametop-fast`).
- **Upkeep:** `scripts/update-check.py` (SteamOS update drift), `scripts/doctor.sh`, models catalog (`models/ft-models`, `catalog.py`, `hands/models.sh`, `voice/models.sh`, `gaze/tracker/models.json`), root bits: `setup/bluetooth` (`steamframe-bt-fixups.service`, system), `/etc/atomic-update.conf.d` keep lists.

### F20. Testbench, MCP and verification skills
- **Does:** fake HMD proximity/pose, controllers, eye and hand trackers, keyboard/mouse into the relay, display power, captures; JSON CLI + stdio MCP server; verify skills drive and model-check Frametop.
- **Processes:** `testbench/frame-testbench` (CLI), `frame-testbench-mcp`; native preload in vrserver (`interpose.cpp`, `input_proxy.cpp`, `control.cpp`, `observe.cpp`) via `~/.config/systemd/user/steamvr.service.d/90-frame-testbench.conf`; control socket `RUN/frame-testbench.sock` (stream, one JSON line); fake trackers write `RUN/frametop-testbench/` (ft-gaze and ft-hands readers prefer them while fresh within 2 s); `frame_testbench/desktop.py` talks to `@frametop_relay`, `@ft_pointer_helper` and `@ft_powerd`.
- **Guard:** `frame_testbench/hostinput.py guard` in transient unit `frametop-fake-worn-guard` (holds every input host's lock; flag `RUN/frametop-fake-worn.json`).
- **Skills:** `.claude/skills/verify-frametop/` (`verify-frametop launch|guard|stream|ask`, `RUN/verify-frametop.json`), `verify-fake-worn-guard/` (FakeWornGuard.tla), `verify-remote-streams/` (untracked: RemoteStreams.tla, HostInputLease.tla).

### F21. Recorders and data contributions
- Hand Recorder (`hands/rec/`, `ft-handrec`, `ft-handpanel` `@ft_handpanel`, bounties, consent, upload), Eye Recorder (`gaze/rec/`, `ft-eyerec`, rounds), shared `recorder/` (age encryption, HF hub upload, bounties, one window, stay awake). Read `cam-ring` / eye camera frames; upload to Hugging Face (DeeJanuz datasets). Hand flags (`ft-handsctl flag`, `HANDS_FLAG_*`) feed `hands/tools`.

### F22. Diagnostics and logging
- `scripts/report.sh`, `report-watch.py`, `gaze-report.py`, `keys-report.py`, `doctor.sh`, `hands/camcheck.py`, `pointer/driver/install.sh probe`; logs: journal per unit, `screens.log` (rotated 8 MB), `/tmp/frametop-{floatd,dashroute,frunner,notifyfwd,gtksettings,tour,remote,vnc}.log`, `RUN/frametop-layout.log`, ft-screens `hands:` lines, ft-stream `hostinput:` lines. No telemetry leaves the Frame.

### Smaller parts
- **Bluetooth fixes** (`setup/bluetooth/`, root): LE address resolution/privacy; Input Settings "Apply Bluetooth fixes" via pkexec.
- **Camera watch** (`ft-camwatch`): warns when SteamVR leaves upper cameras off.
- **Legacy gamescope backend** (`BACKEND=gamescope`): one size per screen, borrows the pointer to arrange.
- **Notifications out / title bars / AT-SPI:** `ft-notifyfwd`, `ft-gtksettings`, `ft-atspi` (in F1's session).

---

## 2. Connections

### 2.1 IPC endpoints (owner -> main clients)

| Endpoint | Kind | Owner | Main clients |
|---|---|---|---|
| `@ft_screens` | datagram, text, reply | ft-screens | ft-layout (incl. `remote N start ...`), ft-floatd, frunner, relay, helper, dashpatch, ft-gaze, ft-handsctl, settings apps, tour, VNC bridge |
| `@frametop_relay` | datagram | input relay | ft-screens, helper, ft-voiced, framekeyboard, ft-textinput, ft-stream, Input Settings, ft-pause, testbench |
| `@ft_pointer_helper` | datagram | ft-pointer | relay, ft-gazed, ft-screens, ft-stream, ft-gazectl, Input Settings, ft-layout, testbench |
| `@ft_pointer` | datagram | ft_pointer driver (in vrserver) | ft-pointer |
| `@ft_gazed` | datagram | ft-gazed | helper, relay, Input Settings, tour, ft-gazectl |
| `@ft_eyes` | datagram | ft-eyes | ft-gazed, probe, bench |
| `@ft_gazepanel` | datagram | ft-gazepanel | gazecheck (in ft-gazed) |
| `@frametop_float` | datagram | ft-floatd | ft-screens, relay, frunner, ft-layout, ft-dashroute, ft-float, tour |
| `@ft_frunner` / `@ft_spotpanel` | datagram | ft-frunner / ft-spotpanel | relay, ft-screens, tour / ft-frunner |
| `@frametop_voice` | datagram | ft-voiced | relay |
| `@ft_powerd` | datagram | ft-powerd | Display Settings, testbench, verify-frametop |
| `@ft_dashpatch` | datagram | ft-dashpatch | ft-screens (`gamepad`) |
| `@ft_handpanel` | datagram | ft-handpanel | Hand Recorder |
| `RUN/frame-testbench.sock` | unix stream, JSON line | testbench preload in vrserver | testbench CLI/MCP, guard |
| seqpacket pair + dmabuf fds | per stream | remote.c | its ft-stream child |
| `RUN/frametop-hands/*` | shm ring + files | ft-camd / ft-hands | ft-hands / ft-screens, helper, ft-handview, recorders; `park` written by relay, `want-hz` by ft-screens |
| `/dev/shm/eye-server.mmap` | SteamVR shm (read-only) | SteamVR eyetracking | ft-gaze |
| `/dev/shm/frametop-eyes-{cams,want,gaze}` | shm/files | ft-eyegrab (cams), ft-eyes (gaze), ft-gazed/ft-eyes (want) | ft-eyes, ft-gaze, Eye Recorder |
| `RUN/frametop-input-HOST.lock`, `frametop-audio-HOST.lock` | flock | one ft-stream per host (or the guard / verify viewer) | ft-stream |
| `RUN/frametop-fake-worn.json`, `RUN/verify-frametop.json` | flag files | testbench / verify-frametop | ft-stream (every tick) |
| `RUN/frametop-pause.json` | state file | relay | relay on restart |
| Steam CEF 8080, SteamVR CEF 8089, vrserver web socket | local debug ports | Steam / SteamVR | ft-steam, steam_settings, ft-dashroute / ft-dashpatch / vrws (relay, voice) |
| KWin D-Bus + KWin script, portal | D-Bus (nested bus) | KWin/Plasma | ft-floatd, ft-gtksettings, ft-notifyfwd (to user bus), ft-layout (scales) |
| systemd user units | process control | systemd | relay (pause stops gaze/hands), handsmode.py, desktops.sh, fast-install |

### 2.2 Who depends on whom (direction of control -> data)

- **SteamVR -> everything in VR.** Units `PartOf=steamvr.service` start/stop with it (pointer, dashpatch, gaze, camd, hands, handview, keyboard, power, autostart). The relay starts **before** SteamVR (uinput devices must exist first). Voice and camwatch are login-scoped.
- **Session -> ft-screens -> KWin.** `frametop-session.sh` starts ft-layout (`start --wait`), ft-screens in the box, then KWin/Plasma on ft-screens' socket; `handsmode up|down` around it. Plasma autostart starts ft-floatd, ft-dashroute, ft-notifyfwd, ft-atspi, tour.
- **Relay = input hub and pause owner.** Devices -> relay -> {ft-screens keys, helper motion, uinput to SteamVR/Steam}. Relay -> pause -> {gaze unit, hands units/park, ft-screens `pause`, VNC, desktop stop}. Helper -> relay (`vrbtn`, `vrgame`). Voice/keyboard/host devices -> relay -> typing target.
- **Pointer helper = pointer and presence hub.** Relay motion + hand gestures + gaze -> helper -> driver pose -> SteamVR laser. Helper owns the worn answer; consumers poll it.
- **Gaze:** SteamVR eye mmap or own tracker -> ft-gaze -> ft-gazed -> helper (`gz`); helper -> ft-gazed (lessons). ft-gazed asks the helper for worn/gaze-on to idle; the relay's pause stops the unit.
- **Hands:** ft-camd -> ring -> ft-hands -> {ft-screens near input + cutouts, helper gestures, ft-handview}; HANDS_MODE via handsmode.py (session) and relay pause (park/stop).
- **Remote:** Remote Displays app / ft-layout -> `@ft_screens` -> remote.c -> ft-stream (fork) -> host. ft-stream -> remote.c (pictures, state). ft-stream -> relay (host devices). ft-stream polls helper (worn) and relay (pause), reads test flags, holds locks.
- **Testbench -> SteamVR boundary.** Fakes proximity, poses, controllers inside vrserver; fake tracker files at ft-gaze/ft-hands' input; desktop commands on the same sockets as real clients. Its worn fake is invisible to the helper, which is why the guard and flag exist.
- **Settings apps -> config files -> daemons** (live via sockets for relay, helper, gazed, screens, powerd; at start for most else).

### 2.3 Shared state and leases
- Host input lock (flock) + 500 ms engage lease (host releases 1.5 s after last).
- Audio lock (flock): one stream per host plays sound.
- Worn answer: freshness 3 s (`kStaleNs`), asked every 1 s by ft-stream.
- Gaze wake lease (Input Settings Gaze page), eyegrab want file (3 s idle).
- ft-dashroute heartbeat (3 s, stale at 10 s) gating dashboard launches.
- Relay fd store (`FileDescriptorStorePreserve=yes`) keeps uinput devices across relay restarts.
- `frametop-pause.json` across relay restarts; desktop started while paused is told on first report.
- ft-screens `hold` (<= 2 s) while ft-floatd resizes.

---

## 3. Known bugs, placed on features

| ID | Status at 403c1eb | Features touched | Where |
|---|---|---|---|
| A | fixed 37d0fbf | F20 testbench CLI, F7 worn, F15 lease | `cli.py` `worn on` dropped the flag after a reply timeout -> faked wearer with no flag -> stream leased |
| B | fixed in ft-stream bd606ce (merged 81a5c82); testbench gap remains, flag covers it | F20 guard, F15 ft-stream | SIGKILL'd guard frees the flock before the wearer is released; ft-stream now reads the flag every tick |
| C | fixed 37d0fbf | F20 | `drop_unsafe_worn` treated a timeout as "no proxy" and cleared the flag |
| D | fixed 0c7eb9e | F20 | `worn on` let `start_guard` clear an unconfirmed-release flag |
| E | fixed 38d6257 | F15 ft-stream, F7 | stale "worn" answer from the faked period leased after the flag went (`hostinput::AfterTest`) |
| F | fixed 1f485fe | F20 guard | guard stopped between `start_guard` and post-check; now leaves the flag |
| (live) | installed release `0.3.0-local.ee053a3` lacks B and E | F15, F19 | needs an install of ft-stream |
| H1 | open (fix in progress, uncommitted) | F15 ft-stream, F7 worn | `NoStaleLease`: a queued "worn" answer gets stamped `now` by `TakeAnswers` after a switch off/on, so `Fresh` accepts it -> engage with nobody wearing it |
| H2 | open (fix in progress) | F15, F14 (stream restart), F7, host fork | `OffOnLiftsEscape`: first stream ends with `Stop(kDrop)`, next stream starts while away and never sends `release` -> escape survives an off-on |
| R1 | open (fix in progress) | F14 remote.c, F3 ft-layout (`remote host ... route=/direct=`), F2 | `OneStreamPerDisplay`: `stop` clears `used` but keeps pid; `start` picks another slot; `host_busy` counts used slots only -> two ft-streams for one display |
| Slot leak | open, unmodelled | F14 remote.c | `stop_stream` sends SIGTERM only, no SIGKILL escalation: an ft-stream that hangs after SIGTERM is never reaped, its slot (pid != 0) never frees |
| Doc drift | open | F15 docs, F2, F14 | `docs/host-input.md` says ft-screens asks the helper, stops streams on headset off and sends `input engage|release`; in code ft-stream asks the helper itself and ft-screens does neither. The doc's own status line says pausing streams isn't built, contradicting its "Headset off and on" and "On the Frame: ft-screens" sections |

---

## 4. Proposed chart groups

Eight groups. Each line: scope / boundaries crossed / model or hand-drawn.

### G1. Lifecycle and presence
- **Scope:** F1 session start/stop, unit ordering with SteamVR, F8 pause for VR games (auto, gesture, manual, resume timing, persistence), F7 worn/away, F9 display power, SteamVR standby.
- **Crosses:** relay (pause owner), helper (worn, `vrgame`), gaze and hands units (stopped/parked), ft-screens (`pause`), VNC, desktop unit, ft-powerd.
- **Method: TLA+ + TLC.** Several writers, restarts and timers: pause x game running x desktop up/down x relay restart x SteamVR restart x worn. Candidate rules: paused => gaze and hands eventually not running; resume restarts only what pause stopped; a desktop started while paused hides; SteamVR restart while paused doesn't leave gaze running; worn readers agree within a bound. Small state space if timers stay abstract. Replaces the hand-drawn session part of the main chart.

### G2. Desktop and panels
- **Scope:** F2 ft-screens panels/controls/visibility/frame rates/pins/spin, F3 layout and profiles, F4 floating windows + frunner + commands, KWin/Plasma helpers (notifyfwd, gtksettings, decoration, atspi), F16 VNC.
- **Crosses:** ft-screens <-> KWin (Wayland, outputs), ft-floatd <-> KWin script, ft-layout, Steam CEF (dashroute), relay (keys).
- **Method: hand-drawn** component/data-flow chart. Mostly request/response and geometry, not races. Optional later: a small TLA+ of float slot accounting (dashlaunch pending count, dock/tear, hold timeout) if slot bugs show up.

### G3. Input and pointer
- **Scope:** F5 relay (roles, grabs, bindings, typing target), F6 helper/driver/dashpatch, F12 keyboards and text input, F13 voice typing path, F8 gamepad mode, controller buttons, laser ownership (mouse vs controller vs hand vs gaze).
- **Crosses:** uinput -> SteamVR/Steam, `@ft_screens` keys, helper <-> driver, dashpatch <-> SteamVR web helper, voice, Full Keyboard, host devices from F15.
- **Method: mixed.** Hand-drawn data flow for routing. TLA+ for two focused properties: held keys always come up on the target they went down on (DEE-222 class: focus change, pause, `hostbye`, `keys release`, screen close) and single owner of each hand's laser (`rayowner.h`, `handoff.h`).

### G4. Gaze
- **Scope:** F10: sources (SteamVR mmap/action, own tracker), ft-gaze -> ft-gazed -> helper, calibration/check panel lifecycle, nudge learning, idle/wake, eyegrab want file.
- **Crosses:** helper (worn, gaze mode, lessons), relay (Meta+J/K, quickcal, pause stops unit), Input Settings (wake lease), root eyegrab, testbench fake eyes.
- **Method: TLA+ for the lifecycle** (idle/wake lease, 30 s stop, headset on -> quick check, calibration open/close/fail, first own-tracker calibration). The 2026-10-05 fresh-install deadlock was exactly this shape. Hand-draw the data path; the numeric parts (fits, fusion) stay out of the model.

### G5. Hands
- **Scope:** F11: camd -> ring -> ft-hands -> {cutouts and near input in ft-screens, gestures to helper, handview}, HANDS_MODE, park during games, side swap, flags, camwatch.
- **Crosses:** XRService cameras, file caps (sudo), session (`handsmode up/down`), relay (park/stop), helper, ft-screens, recorders, testbench fake hands.
- **Method: hand-drawn pipeline.** Optional small TLA+ for "hands run iff HANDS_MODE x desktop x pause x park" if that shows bugs; gesture logic is numeric and belongs in replay tests, not TLC.

### G6. Remote displays and host input
- **Scope:** F14 + F15 + the fake-worn guard side of F20: app -> ft-layout -> remote.c slots -> ft-stream -> host; audio/input locks; lease; relay network devices; mic; slot leak.
- **Crosses:** ft-screens, relay, helper (worn), testbench flag/guard, external Vibepollo/Sunshine and host-input service.
- **Method: TLA+ (exists).** `RemoteStreams.tla`, `HostInputLease.tla`, `FakeWornGuard.tla`. Re-chart from them once H1/R1/H2 fixes land; add the slot leak (SIGTERM hang) to `RemoteStreams` as a new action; fix the doc drift alongside.

### G7. Build, install and release
- **Scope:** F19: dev container, builds, install.sh/get.sh/uninstall.sh, fast-install, OCI image, FrameDrop zip, CI release, update-check, models catalog, Bluetooth root setup, F18 config migration.
- **Crosses:** host vs box, `/etc` + caps (sudo), systemd units, GitHub/Depot, HF models.
- **Method: hand-drawn** flow. Sequential pipelines; `scripts/tests/test_fast_install.py` and release tests cover behavior better than a model would.

### G8. Verification, data and diagnostics
- **Scope:** F20 testbench (preload, control socket, fake trackers, MCP, verify skills), F21 recorders and uploads, F22 reports and logs, tour.
- **Crosses:** vrserver internals, every daemon socket, HF upload, cameras.
- **Method: hand-drawn**, mainly as an "injection points" map: where each fake enters (proximity, pose, eye files, hand files, relay devices) and which real reader it replaces. Its risky piece (fake worn) is already model-checked under G6.

### Suggested charting order
1. **G1 Lifecycle and presence**: everything else hangs on it, and it gives the worn and pause states the other charts reference.
2. **G6 Remote displays and host input**: models exist; redraw after the H1/R1/H2 fixes, adding the slot leak.
3. **G3 Input and pointer**: biggest hub; held-keys and laser-owner models.
4. **G4 Gaze**: lifecycle model.
5. **G5 Hands**.
6. **G2 Desktop and panels**.
7. **G8 Verification, data and diagnostics**.
8. **G7 Build, install and release**.

---

## 5. Open questions and uncertain areas

1. **Two notions of worn.** ft-powerd reads the activity level itself (no 1.5 s rule); the helper applies 1.5 s idle. They can disagree for up to 1.5 s. Harmless today? Should G1 model one worn source or both?
2. **Host fork escape lifetime** (H2 severity): does Vibepollo keep the escape across sessions? Not visible in this repo.
3. **Slot leak reach:** is there any other path that reaps or kills a hung ft-stream (desktop stop pkills ft-screens; does that orphan or kill box children)? Not traced.
4. **Pause x desktop restart x SteamVR restart:** the reference describes recovery paths (desktop told on first report; gaze stopped again when the helper returns) but nothing tests them together. G1's model would.
5. **Grouping calls for Daenon:** worn and pause in G1 (lifecycle) or their own group? Testbench in G8 or overlaid on each chart? VNC in G2 or G6 (both "remote")? Voice in G3 (typing path) or its own?
6. **Gamescope backend:** chart it, or mark it legacy and skip?
7. **Vendored Full Keyboard internals** (`keyboard/`, 244 files) are not mapped beyond its relay link.
8. **Not verified from code here:** the exact port of vrserver's web socket, the full `@ft_screens` remote verbs ft-layout sends, and whether ft-screens itself still reads worn for anything beyond `hands allowed` from the helper.
9. **Uncommitted state:** the H1/R1/H2 fixes in progress and the untracked `verify-remote-streams` skill may change G6 before it is charted.
