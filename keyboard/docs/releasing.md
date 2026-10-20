# Release checklist

The public name is **Full Keyboard for Steam Frame**; the launcher label is **Full Keyboard**. Keep executable names, config paths, overlay identifiers, and package names as `framekeyboard` for compatibility.

## Prepare

- Resolve independent-review findings and update the changelog.
- The project owner sets the version in `CMakeLists.txt` and updates README archive examples. Contributors must leave release versions unchanged; see [version ownership](../CONTRIBUTING.md#version-ownership).
- Run host tests and build the ARM64 package from a matching Frame sysroot.
- Run the safe ARM64 core tests on Frame and record the manual checks in [acceptance](acceptance.md). Do not turn untested items into claims.
- Regenerate the [README image](building.md#refresh-the-readme-image) if the default appearance changed.
- Validate a clean install, update, rollback, and uninstall; preserve user settings.
- Verify same-version reinstall and downgrade into fresh release directories, refusal while the instance lock is held, deletion only after activation/startup confirmation, and preservation of development/unmarked releases. Manual rollback reinstalls an older package; no permanent `previous` link is maintained. Keep a newer installer available to install packages whose bundled installer predates reinstall support.
- Check upgrade compatibility with the previous release's launcher arguments and environment. In-app updates preserve `~/.local/bin/framekeyboard` and the desktop entry, including user customizations; they do not run the new installer. If a release needs new launcher flags or environment variables, keep older launchers working or provide a migration before offering that release through the updater. A manual install refreshes the generated launcher and desktop entry.
- Verify the new package with the previous release's updater helper, which performs the installation before the new helper can run. Preserve the `framekeyboard-VERSION-aarch64.tar.gz` and `SHA256SUMS` asset names, the `framekeyboard-VERSION-aarch64` archive directory, executable `bin/framekeyboard` and `bin/framekeyboard-native-guard`, and at least one JSON profile in each of `layouts`, `languages` and `themes` under `share/framekeyboard`. Keep `--version` printing `framekeyboard X.Y.Z`, `--check --data-dir PATH --config-dir PATH`, and startup confirmation through `FRAMEKEYBOARD_UPDATE_READY_FD` with a single byte `1` within 45 seconds. Format or protocol changes require an upgrade path the old helper already understands.
- Preserve the `--autostart` flag and both startup preference booleans for installations using Start with SteamOS. Verify a hidden boot start, native summon, manual summon, opt-out and update handoff without killing independent helpers.
- Check that LICENSE, third-party notices, README, and docs are included in the archive.
- Inspect tracked files and Git history for private data before making the repository public. Do not publish local sysroots, user profiles, logs, backups, or credentials.

## Package and publish

```sh
export FRAMEKEYBOARD_SYSROOT=/absolute/path/to/frame-sysroot
./scripts/package.sh
```

Upload the generated `out/framekeyboard-VERSION-aarch64.tar.gz` and `out/SHA256SUMS` as assets for the matching tag. Use the archive basename from `SHA256SUMS`; do not rename it after hashing. The checksum file covers that release's install archive, not GitHub's generated source archives.

GitHub publication, repository naming, tags, and release visibility are separate deliberate steps. Preparing a local package does not publish it. No automatic upload workflow is configured.

Describe supported on-device use and known limitations in release notes. Link the README for installation and recovery. If the project has not yet completed a check, state that directly.
