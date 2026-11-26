# Frametop Installer

A window that installs Frametop on a Steam Frame without a terminal. It ships as a Flatpak bundle, `FrametopInstaller.flatpak`, attached to each release. The README's [install section](../README.md#install-on-the-headset) is the user's view; this page is how it works.

## What it does

1. It looks up the two releases it offers: the latest stable one (GitHub's latest release, as `get.sh --release`) and the latest experimental one (the newest public release with a `Frametop.zip`). For each, it shows the version, the download size, and whether it's tested on this SteamOS build, read the same way `install-release.sh` reads it: `main`'s `pack/steamos.json` first, then the release's own `frametop-release.json`. A release that's broken on this SteamOS can't be picked.
2. It lists the optional parts as checkboxes, all ticked. Which parts a release can leave out comes from its `frametop-release.json` (`install_options`, written by `pack/release-info.py` from `install-release.sh --list-options`), or for an older release from that commit's `install-release.sh`, or else from what 0.3.0 took. A part the release always installs shows ticked and locked; one it doesn't have isn't listed. A part that needs another (the Hand Recorder needs hand tracking, our eye tracker needs gaze mode, the Eye Recorder needs our eye tracker) is unticked and locked while the one it needs is unticked.
3. If a kept part needs `sudo`, it asks for the password once and checks it with `sudo -v`. A user who has never set one can set it there (it runs `passwd` on a pseudo-terminal). "Skip those parts" unticks them instead.
4. It runs `get.sh --release --version V --yes` plus the options the picks turn into, as a transient systemd user service, `frametop-installer`, so the install doesn't depend on the window. Its output goes to `~/.cache/frametop-installer/install.log`; the window reads the steps (`== N/13 ...`) and curl's download meter from it for the progress bar.
5. When the install ends, `run-install.sh` writes its exit status to `~/.cache/frametop-installer/result`, and then, if "Restart SteamVR when it's done" was kept, restarts SteamVR.

## Updates

Each time the window opens, it looks for two things on GitHub:

- **A newer Frametop Installer.** A Flatpak bundle has no repository behind it, so neither `flatpak update` nor Discover ever finds a newer one. The installer looks itself (`selfupdate.py`). An installer from an experimental release, or one that finds an experimental Frametop installed, follows the newest public release of any kind that has a `FrametopInstaller.flatpak`. Any other follows GitHub's latest release only, the stable ones. That release is an update when its version is newer than the installer's own (`VERSION` in the bundle, written by `flatpak/build.sh`), and the window offers it before anything else, with Update and Not now. A build with a version no release has (a local build, or the app run from the repo) doesn't look at all.

  Newer goes by the numbers where they say it (`releases.newer`): a higher X.Y.Z, or the same X.Y.Z and a higher experimental build. An experimental build and the stable release of the same X.Y.Z can't be told apart by number, because builds go on under a stable release's number after it's out (`0.3.0-exp.23` came before `0.3.0`, `0.3.0-exp.24` after it). There, the release the installer follows counts as the newer one. So it never goes back to a release the numbers show is older, such as the build before one that was pulled. When the numbers can't show it, a pulled release can send it back: with `0.3.0-exp.28` pulled and `0.3.0` the newest release left, `0.3.0` is offered. The version page's "Update Frametop" line follows the same rule.
- **A newer Frametop than the installed one.** The version page's title becomes "Update Frametop" and says which version is out and which you have, when the newest release on the installed version's channel is newer. Updating is the same install as always, over the old one.

Update downloads the release's bundle into `~/.cache/frametop-installer/update`, checks it against the release's `SHA256SUMS`, and has the host's `flatpak install --bundle` put it where this one is installed. The addresses of both files are built from the release's tag, under Frametop's release downloads on github.com, not taken from GitHub's answer, and a release without `SHA256SUMS` isn't offered. This is the trust an install of Frametop already has: the same releases, over HTTPS. The checksum catches a broken download, not a changed release. Installed for every user of the headset (which is how Discover installs a bundle), flatpak asks for your password through the desktop's own prompt, as it did the first time; installed for this user alone, it doesn't ask. The running window keeps the old files until it closes, so it starts the new version (`flatpak-spawn --latest-version`) and quits. If that fails, it says to open the installer again.

Nothing here blocks an install. With no answer from GitHub, or after Not now, the window goes on as before, and a failed update says why and lets you go on with the version you have. An install that's running isn't interrupted for an update: the window goes straight to its progress. With `--dry-run`, Update downloads and checks the bundle but doesn't install it. Each later look at the version page (Try again, or Change choices after an install) asks GitHub afresh; only the first shares the answers the look for a newer installer got.

## Restarts and crashes

The install's choices are saved in `~/.cache/frametop-installer/state.json` when it starts. On opening, the window checks the service, that file, and the result:

| What it finds | What it opens on |
| --- | --- |
| The service running | The progress page, following the running install |
| A state and no result, and no service (the Frame restarted, or the service was killed) | "The last install didn't finish", with Install again (same version, same choices) and Change choices |
| A result it hasn't shown | Done, or failed with the end of the log |
| Nothing, or a result it has shown | The version page |

Install again is safe: `get.sh` resumes a partial download (`curl -C -`), and the install steps can run again over a finished or half-finished install. An install doesn't start while FrameDrop's install (`frametop-framedrop-install`) is running: the window says so when it opens and again when Install is pressed.

## Debug report

The Debug button in the title bar, on every page, makes a text file to send when something goes wrong: `frametop-debug-<date>.txt` in Downloads. `host/debug-report.sh` collects it on the host, and the window says where it is and who to send it to (`Deej__` on Discord, or dj_anuz@protonmail.com). It has:

- the headset: SteamOS version, build and branch, kernel, memory, disk, heat, battery, whether `sudo` has a password, and whether GitHub answers
- SteamVR's build and its external drivers, the Frametop Installer's own version and Flatpak, and the last install's state, result and log
- the installed Frametop: the release folders, `.frametop-release`, a git checkout in `~/frametop` if there is one, which tree the services run from, the containers and images
- Frametop's services, failed units, what's running, its settings files, and the last two hours' warnings and errors
- with a release installed, its own `scripts/report.sh --system --desktop --gaze --hands` (without `--voice`, which listens to the microphone for a second)

Before it's saved, the values of settings named like a token, secret, password, credential, cookie or auth are hidden, as are passwords in URLs, email addresses, the last two numbers of IPv4 addresses from `10.x.x.x` up, Bluetooth addresses and the headset's serial number. Nothing is sent anywhere. Copy text puts it on the clipboard, for an email; Show in folder opens the file manager on it. Pages that finish while the report is up (an install's progress, a lookup) wait behind it, and Back shows them. When the window won't open, the same report runs in a terminal: `bash ~/.cache/frametop-installer/bin/debug-report.sh` (the scripts are put there the first time the installer does anything on the host).

## The password

The install runs on the host, outside the sandbox's PID namespace, so `sudo` there can't ask the window directly. Instead:

- The window starts `host/helper.py serve` on the host. It listens on `/run/user/UID/frametop-installer/askpass` (a folder only the user can open) and answers only a process in the `frametop-installer` service's cgroup, checked with the socket's peer credentials. That keeps out sandboxed apps, which can't reach the folder, and processes outside the service. It doesn't stop unsandboxed code running as the same user: that code can already move a process into the service's cgroup, or change the scripts in `~/.cache/frametop-installer/bin`, so it's trusted, as it is with `sudo` in a terminal.
- The install runs with `SUDO_ASKPASS` set to `host/askpass`, which asks that socket.
- The password goes from the window to the helper hex-encoded on a pipe. It's never written to a file or put in an argument or the environment, and both drop it when the window closes. If the install asks after the window has gone, askpass fails, and `sudo` treats that as a wrong password: that part fails, the rest goes on.

## The sandbox

The Flatpak's permissions (`flatpak/build.sh`):

| Permission | Why |
| --- | --- |
| `--talk-name=org.freedesktop.Flatpak` | `flatpak-spawn --host`: the install itself, `systemctl --user`, `sudo -v`, `passwd`. This is a full escape from the sandbox, which an installer of user services, a SteamVR driver and root-owned files needs. Flathub would review it. |
| `--filesystem=~/.cache/frametop-installer:create` | The host scripts it copies there (`bin/`), the log, the state, the result, and a downloaded update (`update/`) |
| `--filesystem=xdg-download:ro` | Reading the debug report in Downloads, to copy its text and show it in the file manager (the host script writes it) |
| `--share=network` | GitHub's API and `raw.githubusercontent.com`, for the lookup, and the release's bundle and `SHA256SUMS` for the installer's own update |
| `--socket=wayland`, `--socket=fallback-x11`, `--device=dri`, `--share=ipc` | The window |

Starting the new version after an update needs no permission of its own: `flatpak-spawn --latest-version` goes through the Flatpak portal every app can use. Host commands run with `LD_PRELOAD` and `LD_LIBRARY_PATH` unset, and with the host's `PATH`, `HOME` and `XDG_RUNTIME_DIR`.

## Files

- `frametop_installer/`: the app (Python, GTK 4, libadwaita when present). `options.py` holds the parts and what each release can leave out, `releases.py` the lookup and the order of versions, `selfupdate.py` the installer's own update, `host.py` the host commands, and `job.py` the service, its state and the progress. The window is `ui.py` (the version, parts and resume pages), with `password_pages.py`, `progress_page.py`, `update_page.py` and `debug_page.py` mixed into it, `askpass.py` for its end of the askpass socket, and `widgets.py` for the small GTK pieces.
- `host/`: what runs on the host. `helper.py` (askpass socket, `passwd`), `askpass`, `run-install.sh` (result, then the SteamVR restart), `debug-report.sh` (the Debug button's report), and `fake-install.sh` for test runs.
- `flatpak/`: the launcher, desktop entry, AppStream metadata, icon, and `build.sh`.
- `tests/`: offline tests, in `just test` through `scripts/testgate/python-suites.txt`.

## Build and try it

`installer/flatpak/build.sh` builds `installer/build/FrametopInstaller.flatpak`. It needs `flatpak` and `org.gnome.Platform//51` for the target architecture, and nothing compiles, so it builds on the Frame itself:

```
flatpak install --user flathub org.gnome.Platform//51   # SteamOS usually has it already
installer/flatpak/build.sh
flatpak install --user installer/build/FrametopInstaller.flatpak
flatpak run io.github.frametop.Installer --dry-run
```

`--dry-run` (or `FRAMETOP_INSTALLER_DRY_RUN=1`) goes through every page but runs `host/fake-install.sh` in place of `get.sh`: nothing downloads or installs, the password isn't checked, and SteamVR isn't restarted. It still asks for a password when a part needs it, and the fake install calls askpass once, so the socket is tested too. `FRAMETOP_INSTALLER_NO_PASSWORD=1` with it shows the set-a-password page. Outside the Flatpak, `cd installer && python3 -m frametop_installer --dry-run` runs the same code.

Release CI builds the bundle on arm64 (`.github/workflows/release.yml`), attaches it to the draft release, and lists it in `SHA256SUMS`.
