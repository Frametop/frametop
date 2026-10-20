# Runtime and troubleshooting

## Launch and dashboard behavior

The installed **Full Keyboard** launcher uses:

```sh
framekeyboard --vr --input ei --start-enabled
```

It connects to the running Frame Gamescope compositor at `/run/user/<uid>/gamescope-0-ei`. The headset's VR session must already be running. The app does not start SteamVR or register autostart. Stock keyboard replacement is an experimental option, disabled by default. It is visible and accepts input only while the dashboard is open. Hiding cancels keys, unfinished composition, and grabs.

First launch starts at 95 cm wide, tilted back 50° from upright, just beneath the dashboard. If the dashboard anchor is unavailable, it falls back to 65 cm below eye level and 85 cm forward. Placement follows dashboard position and full rotation, including when a local app tab is selected.

Quitting saves the pose, size, and dashboard reference. Reopening restores them. Launching an already running instance recenters it and preserves size; it also returns from Settings to the keys. A launch while the dashboard is hidden queues recentering for its next opening. Later launch flags do not change the running instance's backend or language. Quit it before changing those options.

When Experimental → Replace native keyboard is enabled, × dismisses the current typing session and hides the panel while the app stays resident. The next supported native keyboard request reopens our panel. **Quit Full Keyboard** is available only in Settings while replacement is enabled; use it for a full exit before updates, removal or changing launch options. With replacement disabled, × exits normally. See [native replacement](native-replacement.md).

A temporary compositor pause releases held keys and resumes typing when its keyboard device returns. After a disconnect, native and Unicode connections retry in the background with a 1-second delay doubling up to 30 seconds. Old holds and text are never replayed. The "Keyboard paused or disconnected" status remains visible even if the interruption happened while the dashboard was closed. OpenVR session loss can still require a relaunch.

Tracking loss hides the keyboard and releases input. When tracking returns, the panel returns to its pose. A tracking-origin reset or a known saved tracking-space mismatch recenters it while retaining size. Corrupt saved placement is ignored. Forced termination cannot save the latest movement.

Keys have no fixed hold deadline: holding Shift with one hand or repeating Backspace can continue beyond ten seconds. A known controller losing tracking or leaving the panel cancels only its pointer; hiding, application-focus loss, and shutdown release all keys.

## Movement and feedback

By default, point at the line below the keyboard, hold the trigger, move your hand, then release to place it. The line matches the native dashboard handle: dark gray at rest, white and 10% wider while hovered or held, with an 80 ms transition. To use the original grip movement, enable Use Drag to Move in the General settings card and Apply. In that mode the line is hidden; point at the keyboard and squeeze grab/grip to move and rotate it. While holding the movement line or grabbing, that hand's thumbstick moves it farther with up and closer with down. Line movement does not require holding grip. Grip must be released once after launch or a controller reconnect. Either controller can start movement, and only that pointer owns it until release. The AUX head cursor can also capture the line: aim with your head, hold AUX, move your head and release. Head tracking supplies its pose; haptics remain controller-only. Pinning disables both methods and hides the line. Tracking loss, hiding, recentering, or a 30-second movement timeout restores the typing state.

The keyboard reads grip travel from the native Frame controller model. Other controller models are unsupported. Steam updates that change this model may require an app update. No global SteamVR input setting is changed.

General → Auto leveling is on by default. Horizon assistance runs only after release: a roll within 5° eases level over 500 ms without changing pitch. Disable Auto leveling and Apply to retain your chosen roll. Changing the setting preserves the current displayed pose. Toolbar size controls change width by 5 cm within 45 cm to 2 m.

Press feedback uses 25 ms at full strength; release uses 8 ms at 35%, both at 150 Hz. Hover uses 4 ms at 10% and 240 Hz. Feedback goes to the controller responsible for the interaction. Toolbar actions, repeats, and dragging do not pulse.

## Language and input routing

Choose a language in Settings and Apply and save. The normal `--input ei` path
sends locally resolved Unicode characters, so system layouts and launch-time
language declarations do not control typing. Native Enter, arrows and shortcuts
remain available. See [language selection](languages.md).

Only the developer uinput backend and external Japanese JIS mode require a
matching target declaration. For an existing Japanese system keymap and IME,
select the JIS preset and launch with `--target-language ja-jis`. The optional
`FRAMEKEYBOARD_TARGET_LANGUAGE` launcher variable exists for that physical mode.

Unicode text follows the matching Wayland socket: for example, `/run/user/1000/gamescope-2-ei` pairs with `/run/user/1000/gamescope-2`. Relative names resolve under `$XDG_RUNTIME_DIR` in both backends. For nonstandard names, add `--text-socket PATH` explicitly; otherwise Unicode text is unavailable rather than being sent to the default compositor. `--text-socket` requires `--input ei`. Close the running keyboard before changing launch options, since launching a second instance only recenters the first.

For example, to address a different running compositor:

```sh
~/.local/bin/framekeyboard --vr --input ei --start-enabled \
  --ei-socket gamescope-2-ei
```

The optional `--input uinput` developer backend requires access to `/dev/uinput`; newly created devices were not discovered by the tested outer VR session. The installed launcher uses libei and does not need this backend.

Copy/Paste send Ctrl+C/Ctrl+V. They do not synchronize clipboards between sessions, implement terminal Ctrl+Shift shortcuts, or read clipboard data. In Unicode mode Caps/Num Lock are local to this panel and do not change the system locks. Native modifier shortcuts remain subject to receiving-app behavior.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Keyboard is missing | Open the dashboard, wake the headset, then launch Full Keyboard again to recenter |
| Keyboard is offscreen | Relaunch while it is running, or use the recenter icon |
| App exits immediately | Run `~/.local/bin/framekeyboard` in a terminal and read the error; check that the VR session is running |
| Keys do not reach a local app | Focus a harmless text field first; check the Unicode text connection and chosen profile |
| Keyboard paused or disconnected | Wait for the compositor; native and Unicode connections retry in the background and old holds are cleared |
| Input connection was lost | Wait for background reconnect; old holds and text are discarded. For an unrecoverable resident failure, use Settings → Quit Full Keyboard, then relaunch |
| Wrong characters | Check the selected app language and custom XKB profile; physical uinput/external-JIS modes additionally need a matching target keymap |
| Copy/Paste fail in a terminal | This version sends Ctrl+C/Ctrl+V, not terminal Ctrl+Shift shortcuts |
| Grip does nothing | Release it fully, point at the keyboard, then squeeze; wake the controller and verify it is a Frame controller |
| Japanese only previews text | Check Anthy, fonts, the native text protocol, and the selected mode; see [Japanese input](japanese.md) |
| Installer says Full Keyboard is running | Quit Full Keyboard in Settings, then run the installer again |
| New name is absent in Steam Library | A manually added shortcut has its own title; rename it to Full Keyboard in its Properties |

No input is sent outside the dashboard. Streamed VR applications, including VRChat, are outside the supported scope. Focus behavior in nested desktops and floating apps needs testing per application. Do not assume success in a dedicated receiver proves every app works.

## Update, rollback, and removal

Use the centered Settings → Check for updates button for a verified download, installation and restart. See [updates](updates.md) for button states, requirements and recovery.

For a manual installation, quit Full Keyboard first. The installer holds the instance lock throughout installation and refuses if the app is running. It validates the copied ARM64 binary and profiles in a fresh staging directory before switching `current`, then removes the replaced production release. Settings under `~/.config/framekeyboard` are preserved. The same version can be reinstalled, and older versions can replace newer ones. A failed preparation leaves the current release usable; interrupted cleanup never deletes the release named by `current`.

To roll back, download an older release archive and its checksums, verify it and extract it as described in the README. Before rolling back to a release without Start with SteamOS, disable that option and apply it first. Quit Full Keyboard, then reinstall the older package. For an older package whose installer predates reinstall support, use the installer from a newer extracted package and pass the older package directory:

```sh
bash /path/to/newer-package/install.sh /path/to/extracted-older-package
~/.local/bin/framekeyboard
```

Production installs keep no permanent rollback release or `previous` link. During an in-app update, the old release and preference snapshot survive until the new process confirms startup; a failed start restores the old release automatically. Cleanup only removes directories positively marked by the production installer/updater. Development deployments, historical unmarked releases and unrelated `previous` links stay untouched. Older installers/updaters keep their original retention behavior. The installed app label stays Full Keyboard when reinstalling an older binary.

To uninstall, first uncheck Start with SteamOS and Apply and save, then quit the app. If the app cannot run, remove its optional startup unit first:

```sh
systemctl --user disable --now framekeyboard-autostart.service
rm -f ~/.config/systemd/user/framekeyboard-autostart.service
systemctl --user daemon-reload
```

Then run:

```sh
rm ~/.local/bin/framekeyboard
rm ~/.local/share/applications/framekeyboard.desktop
rm -r ~/.local/share/framekeyboard
```

Keep `~/.config/framekeyboard` if you want to preserve themes, layouts, and placement. If you added a Steam Library shortcut manually, remove it through Steam too. No OS or stock keyboard files need restoring. The optional user-local startup unit is removed as described above.

## Developer diagnostics

Desktop preview and PNG export never inject input. A direct `--vr` binary launch also starts input-disabled unless a backend and `--start-enabled` are supplied. Normal compositor input does not require `--target-language`; physical uinput and external Japanese JIS mode still require a matching target declaration. For an input-disabled installed VR check, close any running instance first, then use:

```sh
~/.local/bin/framekeyboard --vr --input none --duration 4 --config-dir /tmp/full-keyboard-smoke
```

A second launch only recenters the first instance; it cannot turn an active typing session into a safe test session. Core CTest suites use fake sinks. Input probes are opt-in and require a dedicated receiver or their own exclusively grabbed device. See [building and tests](building.md).

See [languages and system keyboard settings](languages.md) for all profile IDs, Chinese/Korean controls, and the explicit limitations when multiple system layouts are configured.
