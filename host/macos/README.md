# Frametop host for macOS

This kit makes a Mac a host for Frametop's remote displays: the Mac's monitors appear as screens on a Steam Frame. Vibepollo has no macOS build, so the Mac uses LizardByte's Sunshine, one copy (an instance) per display. Frametop Streams, a menu bar icon, turns them on and off.

## Requirements

- A Mac with Apple silicon, on macOS 14.2 or later.
- An admin account, to install Sunshine in /Applications.
- The Mac and the Steam Frame on the same network.
- Frametop on the Steam Frame.

## Set up

1. Download `frametop-macos-host-1.zip` and open it. You get a folder named `frametop-macos-host`.
2. Open Terminal and run the setup:

   ```
   bash ~/Downloads/frametop-macos-host/frametop-host-setup.sh
   ```

   You can also double-click `Setup Frametop host.command`. The kit isn't signed, so macOS blocks it the first time. To allow it, open System Settings > Privacy & Security, click Open Anyway next to the message about it, then open it again.
3. The setup does the following:
   - It installs Sunshine 2026.1010.141104 in /Applications after checking the download's SHA-256. This is a pre-release, and it's the first build where the mouse reaches displays other than the main one.
   - It creates one Sunshine instance per display, on ports 47989, 48089, 48189 and so on, each started at login.
   - It asks for the Web UI user name and password that the Steam Frame signs in with. The password is typed in Terminal and is never shown or saved. If an earlier run of this setup or an earlier Sunshine install already has a login, the setup uses that one instead.
   - It adds Frametop Streams to the menu bar.
4. When macOS asks, allow Sunshine both Screen Recording and Accessibility in System Settings > Privacy & Security. Then go back to Terminal and press Enter. Without Accessibility, the pointer moves but clicks, scrolling and typing don't reach the Mac. The setup checks both permissions and offers to ask again if Accessibility is missing.
5. On the Steam Frame, open Frametop Remote Displays and choose Add computer. Pick this Mac, sign in with the Web UI login, and choose the displays to show.

Run the setup again after you connect a new monitor.

## Turn the streams on and off

Click the display icon in the menu bar to open Frametop Streams. It lists the Mac's displays and puts a check next to each one that's on. Click a display to turn it on or off, or choose Turn All On or Turn All Off. The icon is dimmed while every display is off. A display you turn off stays off after a restart, until you turn it on again.

You can do the same in Terminal with `ft-sunshine`:

```
ft-sunshine status    # each display and whether it's on
ft-sunshine off       # turn every display off
ft-sunshine on        # turn every display on
ft-sunshine on 1      # turn on one display, by the number status shows
```

If you have a `~/.local/bin` folder, the setup puts `ft-sunshine` there. Otherwise, run it as `~/Library/Application\ Support/Frametop/sunshine/bin/ft-sunshine`.

While a display is off, the Steam Frame keeps trying to reach it, and it reconnects once the display is on again.

If you quit Frametop Streams, it starts again at your next login. You can also open it from Spotlight.

## Good to know

- The Steam Frame can't reach a Mac that's asleep. To keep the Mac awake on power, turn on System Settings > Battery > Options > Prevent automatic sleeping on power adapter when the display is off. The setup tells you if the Mac sleeps when idle.
- Sunshine's own menu bar icon belongs to the main display's instance. Quitting Sunshine from that icon stops only the main display. Frametop Streams or `ft-sunshine on` starts it again.
- Logs are in `~/Library/Logs/Frametop`.

## Check and remove

- `bash frametop-host-setup.sh --check` shows what the setup would change, without changing anything.
- `bash frametop-host-setup.sh --undo` removes the instances' LaunchAgents, Frametop Streams and `ft-sunshine`. Sunshine and its settings and pairings stay, and the setup says how to remove those too.

## Source

This kit is `host/macos` in [Frametop](https://github.com/Frametop/frametop), under the MIT license. Sunshine is [LizardByte's](https://github.com/LizardByte/Sunshine), under GPL-3.0. The setup downloads it from Sunshine's GitHub release.
