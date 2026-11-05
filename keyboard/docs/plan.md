# Roadmap

Full Keyboard for Steam Frame is a manually launched alternative keyboard for local dashboard apps. Default behavior leaves the system keyboard untouched. An experimental, default-off replacement bridge handles supported native requests while the app runs. Streamed VR-app input is outside the project scope.

## Implemented

- Native ARM64 overlay, libei key delivery, desktop preview, and image export.
- Full-size US, international, and Japanese layouts with editable JSON profiles.
- European/Cyrillic XKB profiles, Brazilian ABNT2 geometry, integrated Japanese, Simplified/Traditional Pinyin and Korean two-set composition, plus external JIS mode.
- In-VR settings, theme selection, favorites, reload, and persistent configuration.
- Manual stable-release checks and verified, staged update/restart with rollback. See [updates](updates.md); future-release headset acceptance remains pending.
- Laser-trigger movement from a line below the keyboard, optional grip movement, same-hand stick depth in either controller mode, resize icons, and optional release-only horizon alignment, enabled by default.
- Dashboard position/rotation following and dashboard-only visibility/input.
- Saved placement, single-instance launches, and relaunch-to-recenter.
- Per-controller haptics, cancellation paths, host tests, and ARM64 release packaging.

## Before the first public release

- Independent code review and resolution of actionable findings.
- Review the [release checklist](releasing.md) and [manual acceptance matrix](acceptance.md).
- Confirm install/update/uninstall instructions from a fresh release archive.
- Review the public repository and package for private files and complete third-party notices.
- Publish a release with the ARM64 archive and checksums once review is complete.

## Experimental native replacement

- [x] Default-off setting, temporary bridge, request readiness and independent native fallback.
- [x] Resident panel dismissal with × and a separate Settings Quit shown only in replacement mode.
- [x] Optional Start with SteamOS, dependent on replacement, with hidden background startup.
- [x] Isolated request, timeout, target-switch, Unicode and cleanup regressions.
- [ ] In-headset automatic Steam-field, dashboard summon and zero-flash acceptance.
- [ ] Crash, hang, backend loss and sleep/wake acceptance with usable native recovery.
- [ ] Buffered requests and broader native target coverage.

## Further work

- Broader local-app, nested-desktop, and language testing.
- Fluent Japanese-user feedback on composition and candidate controls.
- Broader application testing for Unicode delivery and native shortcuts across system keymaps.
- Sleep/wake, runtime-update, and longer-session testing.
- Measured battery, memory, and latency results.

No universal native keyboard replacement, streamed PC input, speech recognition, swipe typing, or cross-session clipboard synchronization is promised. See [changelog](../CHANGELOG.md) for released behavior changes.
