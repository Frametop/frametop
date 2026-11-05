---
title: Install the Frametop Hand Recorder
---

# Install the Frametop Hand Recorder

The Hand Recorder records your hands with the Steam Frame's tracking cameras for Frametop's open hand dataset ([DeeJanuz/frametop-hands](https://huggingface.co/datasets/DeeJanuz/frametop-hands) on Hugging Face), which trains Frametop's hand tracking.

You pick a **bounty**: a short round, about 4 minutes, of what the hand tracker needs most right now. Windows appear in front of you in the headset, and you poke, tap, pinch and grab them as you would real ones while the cameras record your hands. Afterwards you watch what was recorded, delete anything you don't want to share, and upload it from your own Hugging Face account. Nothing leaves the headset until you press Upload.

With Frametop's hand tracking installed there's also a **testing session**: up to 3 minutes of using Frametop as you always do. When the tracking gets something wrong, press the headset button to mark the moment. New versions of the hand tracking are tested on these recordings before they ship. Testing sessions are never used for training and aren't published: they're encrypted on your headset so that only the maintainer can open them.

Join the [Frametop Discord](https://discord.gg/W3X9f7z3Bc) for questions and help with recording.

There are two ways to install it. Both record the same rounds for the same dataset.

- [**Without Frametop**](#without-frametop): the standalone Hand Recorder runs from SteamVR's dashboard. One command installs it.
- [**With Frametop**](#with-frametop): Frametop's experimental release brings the recorder already built. It runs on Frametop's desktop.

## Before you start

- You must be 18 or older, and for now you can't take part if you live in Illinois, Texas or Washington (USA). The [consent text](https://github.com/Frametop/frametop/blob/experimental/hands/rec/CONSENT.md) explains what's recorded and what you agree to. The recorder shows it again before your first round.
- You need a Steam Frame on the stable SteamOS release (not the beta), an internet connection, and a keyboard (Bluetooth, or the on-screen one).
- You need a `sudo` password: the camera broker needs it once to read the cameras. If you've never set one, run `passwd` in Konsole first.
- A round and its export take up to about 30 GB. You can delete both once the round is uploaded. `df -h ~` shows your free space.

## Without Frametop

1. In the headset, open the dashboard, then **Launch a program** → **Konsole**.
2. Run:

   ```
   curl -fsSL https://raw.githubusercontent.com/Frametop/frametop-hand-recorder/main/get-hand-recorder.sh | bash
   ```

   It asks for your `sudo` password once.
3. Open the dashboard, then **Launch a program** → **Frametop Hand Recorder**.

To update, run the command again: it installs the newest release. To uninstall:

```
~/.local/share/frametop-hand-recorder/current/get-hand-recorder.sh --uninstall
```

The [standalone recorder's README](https://github.com/Frametop/frametop-hand-recorder#readme) says what the installer changes.

## With Frametop

1. In the launcher, choose Launch a program → Desktop.
2. In the application menu, open System → Konsole.
3. Run:

   ```
   curl -fsSL https://frametop.github.io/frametop/get.sh | bash -s -- --release --experimental
   ```

   This downloads Frametop's newest experimental release (about 1.2 GB) and installs it without compiling anything. The installer asks what to install: say yes to the Hand Recorder, and to hand tracking if you want to record testing sessions. It asks for your `sudo` password for those. At the end SteamVR restarts, which closes Konsole. If Frametop is already installed, the same command updates it, and the Hand Recorder with it.
4. Choose Launch a program → Desktop again, and open Frametop Hand Recorder from the application menu.

Installing `Frametop.zip` with [FrameDrop](https://framedropvr.com), or by hand from the [releases](https://github.com/Frametop/frametop/releases), works too: leave Hand Recorder ticked in the installer's window.

To add the recorder to a release you've already installed:

```
~/.local/share/frametop/releases/current/hands/rec/install.sh
```

From a clone of the experimental branch (`get.sh --experimental`, without `--release`), `~/frametop/hands/rec/install.sh` builds it, which takes a few minutes. Frametop's stable version (0.2.2) has an older recorder without bounty rounds.

## Record

Open the recorder. It starts on **Bounties**, with the rounds needed most at the top. Press **Record** on one. The first time, it shows the consent text. Then a short checklist checks the cameras, and the round starts in the headset.

While you record, use the button on the right side of the headset: press it to start each step, press it twice to record a step again, and hold it to stop. In a testing session, a press marks a moment and a hold finishes.

After the round, **Export and upload** prepares it and opens the upload page, where you log in to Hugging Face. Your upload arrives as a pull request on the dataset, and the maintainer checks it before it's merged.

## Uninstall

For the standalone recorder, see [Without Frametop](#without-frametop). For Frametop's, run its installer with `uninstall`: `~/.local/share/frametop/releases/current/hands/rec/install.sh uninstall` for a release, or `~/frametop/hands/rec/install.sh uninstall` for a clone.

This removes the menu entry. With your `sudo` password, it also takes back the camera broker's permission to read the cameras. If you also installed Frametop's live hand tracking, the camera broker keeps that permission, because live hand tracking still uses it. Your recordings stay in `~/.local/share/frametop/hands/contrib`; delete that folder to remove them. To remove Frametop as well, follow [Uninstall](https://github.com/Frametop/frametop#uninstall) in the README.

## Help

Ask in the [Frametop Discord](https://discord.gg/W3X9f7z3Bc), the [Frametop issues](https://github.com/Frametop/frametop/issues), or the dataset's [discussion page](https://huggingface.co/datasets/DeeJanuz/frametop-hands/discussions). All three are public.
