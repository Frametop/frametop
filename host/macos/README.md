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

### Monitors that come and go

Each instance belongs to one monitor, found by its maker, model and serial number, not by its place or the number macOS gives it. When that monitor is unplugged or the lid closes, its instance stops and waits, and starts again by itself once the monitor is back. Frametop Streams and `ft-sunshine status` show it as waiting. A monitor the Mac hasn't had before gets no instance on its own: run the setup again after you connect one (or set its instance up [by hand](#set-up-the-streams-by-hand)), and then add it in Remote Displays on the Steam Frame. Nothing makes virtual monitors on a Mac: Sunshine can't on macOS.

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

## Set up the streams by hand

The setup does all of this for you. These steps are for setting up an instance yourself, for example to see what the setup does or to fix one display. Each display needs its own Sunshine instance, with its own settings file, ports and display. The Steam Frame looks for instances on ports 47989, 48089 and so on, up to 48689, so a Mac can stream up to eight displays.

1. Install [Sunshine 2026.1010.141104](https://github.com/LizardByte/Sunshine/releases/tag/v2026.1010.141104) (`Sunshine-macOS-arm64.dmg`) in /Applications. Older releases move the mouse only on the main display.
2. List the displays, from the kit's folder:

   ```
   osascript -l JavaScript displays.js
   ```

   Each line is one display: its id, its key (maker, model and serial number), its name, its width and height in pixels, and its place. Sunshine wants the id. The id can change when a monitor is plugged in again; the key doesn't.
3. Make one certificate for every instance, so the Steam Frame sees one Mac rather than one per display:

   ```
   dir="$HOME/Library/Application Support/Frametop/sunshine"
   mkdir -p "$dir/credentials" ~/Library/Logs/Frametop
   (umask 077; openssl req -x509 -newkey rsa:2048 -nodes -days 7300 -subj "/CN=Sunshine Gamestream Host" \
     -keyout "$dir/credentials/cakey.pem" -out "$dir/credentials/cacert.pem")
   ```

4. For each display, pick a number N from 0 to 7. Make a folder `instance-N` in that folder, with this `sunshine.conf` in it. Replace N, the port (47989 + 100 x N), the names, the size and the display's id:

   ```
   sunshine_name = My Mac: DELL U2723QE (3840x2160)
   port = 48089
   output_name = 2
   file_state = /Users/you/Library/Application Support/Frametop/sunshine/instance-1/sunshine_state.json
   credentials_file = /Users/you/Library/Application Support/Frametop/sunshine/credentials.json
   file_apps = /Users/you/Library/Application Support/Frametop/sunshine/instance-1/apps.json
   log_path = /Users/you/Library/Logs/Frametop/sunshine-1.log
   pkey = /Users/you/Library/Application Support/Frametop/sunshine/credentials/cakey.pem
   cert = /Users/you/Library/Application Support/Frametop/sunshine/credentials/cacert.pem
   minimum_fps_target = 1
   system_tray = disabled
   ```

   - Keep the name's form, `COMPUTER: DISPLAY (WIDTHxHEIGHT)`. Remote Displays shows the part after the colon, and reads the display's size from the end.
   - Every instance shares the one `credentials_file`, so one Web UI login works for all of them.
   - `minimum_fps_target = 1` lets a still desktop drop to 1 frame a second.
   - Turn `system_tray` on for one instance only, or you get one Sunshine icon per display.

   Next to it, put an `apps.json` with one app named Desktop, which the Steam Frame opens:

   ```
   {"env": {}, "apps": [{"name": "Desktop", "image-path": "desktop.png"}]}
   ```

5. Set the Web UI login once. It's saved in the shared `credentials_file`:

   ```
   /Applications/Sunshine.app/Contents/MacOS/Sunshine "$dir/instance-0/sunshine.conf" --creds USER PASSWORD
   ```

6. Start each instance:

   ```
   /Applications/Sunshine.app/Contents/MacOS/Sunshine "$dir/instance-0/sunshine.conf"
   ```

   Its Web UI is on the next port up from its `port`, for example https://localhost:47990 for instance 0. To start it at login, make a LaunchAgent that runs the same command. The setup's agents run `ft-sunshine-instance N` instead, which looks the display's id up by its key at every start and waits while the display is unplugged. A hand-written `output_name` is fixed, so check it after you plug monitors in again: when it names no display, Sunshine streams the main one.
7. Allow Sunshine Screen Recording and Accessibility in System Settings > Privacy & Security, as in the setup's step 4.
8. On the Steam Frame, choose Add computer in Frametop Remote Displays, pick the Mac and sign in. It lists every instance that answers.

To add a monitor later, give it the next free number and repeat steps 4 and 6. Don't mix this with the setup on one Mac: the setup keeps track of its instances by a `display` file it writes in each folder, so it can give a hand-made instance's number to another display and rewrite that instance's settings.

## Source

This kit is `host/macos` in [Frametop](https://github.com/Frametop/frametop), under the MIT license. Sunshine is [LizardByte's](https://github.com/LizardByte/Sunshine), under GPL-3.0. The setup downloads it from Sunshine's GitHub release.
