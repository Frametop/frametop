## Install

On a Steam Frame (SteamOS, VR variant). Download **Frametop.zip** below (about 1.2 GB), then either:

- **With [FrameDrop](https://framedropvr.com) on a PC:** drop Frametop.zip on your paired headset. In the headset, open Frametop in your library and press Play.
- **On the headset:** unpack Frametop.zip (in the desktop's Dolphin or Konsole) and run `Frametop/frametop-install.sh`.
- **In a terminal on the headset:** `curl -fsSL https://frametop.github.io/frametop/get.sh | bash -s -- --release`

Or skip the zip: download **FrametopInstaller.flatpak** below on the headset's desktop, open it from Downloads, then open Frametop Installer from the application menu. It downloads and installs the release.

A window asks what to install, and your SteamOS password for the optional parts that need it (hand tracking, the Hand Recorder, our own eye tracker, and the Bluetooth fixes); the password is only used for this install. Nothing is compiled on the headset. Restart SteamVR once afterwards. In the terminal, options after `--release` answer ahead: `--together` or `--no-together` for Frametop Together, and `--no-keyboard` to keep a Full Keyboard you run yourself.

Before installing, the installer checks your SteamOS build against the builds Frametop was tested on, and stops if this release is known not to work there.

To show another computer's monitors as screens (remote displays), set that computer up once. On a Windows or Linux PC, install Vibepollo from our fork: [Frametop/frametop-vibepollo](https://github.com/Frametop/frametop-vibepollo). A Mac uses its [host kit](https://github.com/Frametop/frametop/releases/tag/macos-host-1).
