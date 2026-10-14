## Install

On a Steam Frame (SteamOS, VR variant). Download **Frametop.zip** below (about 1.1 GB), then either:

- **With [FrameDrop](https://framedropvr.com) on a PC:** drop Frametop.zip on your paired headset. In the headset, open Frametop in your library and press Play.
- **On the headset:** unpack Frametop.zip (in the desktop's Dolphin or Konsole) and run `Frametop/frametop-install.sh`.
- **In a terminal on the headset:** `curl -fsSL https://frametop.github.io/frametop/get.sh | bash -s -- --release`

A window asks what to install, and your SteamOS password for the three optional parts that need it (hand tracking, our own eye tracker, and the Bluetooth fixes); the password is only used for this install. Nothing is compiled on the headset. Restart SteamVR once afterwards.

Before installing, the installer checks your SteamOS build against the builds Frametop was tested on, and stops if this release is known not to work there.

To show another computer's monitors as screens (remote displays), set that computer up once with its host kit: [Windows](https://github.com/Frametop/frametop/releases/tag/windows-host-1), [Mac](https://github.com/Frametop/frametop/releases/tag/macos-host-1), or [Linux](https://github.com/Frametop/frametop/blob/experimental/host/linux/README.md). The README's [Remote displays](https://github.com/Frametop/frametop/blob/experimental/README.md#remote-displays) section has the steps.
