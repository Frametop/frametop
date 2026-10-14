# Frametop host for Windows

This kit makes a Windows PC a host for Frametop's remote displays: the PC's monitors appear as screens on a Steam Frame, and it can also make virtual monitors at any size. The PC streams with Vibepollo, a version of Sunshine, and Frametop's build of Vibepollo's `sunshine.exe`.

## Requirements

- Windows 10 or 11, 64-bit.
- A graphics card with a hardware video encoder. It's tested on an NVIDIA card.
- An admin account. The setup asks Windows for admin, so you confirm one prompt.
- The PC and the Steam Frame on the same network, or a Steam Link dongle plugged into the PC (see [Stream over a Steam Link dongle](#stream-over-a-steam-link-dongle)).
- Frametop on the Steam Frame.

If Sunshine runs on this PC, uninstall it first: Vibepollo uses the same ports. If Apollo is installed, Vibepollo's installer replaces it and keeps its settings and pairings.

## Set up

1. Download `frametop-windows-host-1.zip` from the [`windows-host-1`](https://github.com/Frametop/frametop/releases/tag/windows-host-1) release and extract it. You get a folder named `frametop-windows-host`.
2. Double-click `Setup Frametop host.cmd` in that folder. Windows may warn that the file came from the internet: choose Run. Then allow the admin prompt.
3. The setup does the following:
   - If Vibepollo isn't installed, it downloads Vibepollo 2.0.0, checks the download's SHA-256 and runs Vibepollo's installer. Follow the installer's steps, then go back to the setup window.
   - It replaces Vibepollo's `sunshine.exe` with Frametop's build, after checking its SHA-256. The original is kept next to it as `sunshine.exe.2.0.0-original`.
   - It changes three Vibepollo settings: remote displays carry the PC's sound, and a virtual display goes away when Frametop disconnects it but stays through a dropped stream. Your other settings stay as they are.
   - It asks for the Web UI user name and password that the Steam Frame signs in with. If Vibepollo already has a login, you can keep it. The password is typed in the setup window and is never shown or saved.
   - It lets Vibepollo through the firewall on every network type, says whether a Steam Link dongle is plugged in, and checks that the Web UI answers.
4. When it says Done, it prints this PC's name and addresses. On the Steam Frame, open Frametop Remote Displays and choose Add computer. Pick this PC, sign in with the Web UI login, and choose the displays to show: its monitors, or a virtual display at the size you pick.

When a newer kit comes out, run its setup the same way. It replaces an older Frametop build.

## What Frametop's build changes

Frametop's build is Vibepollo 2.0.0 with a few changes, from [Frametop/frametop-vibepollo](https://github.com/Frametop/frametop-vibepollo):

- It can stream any of the PC's monitors, not only the main one, and several at once.
- While a monitor streams, it switches to the stream's resolution (or the closest mode it has with the same shape), and back when the stream ends. Set the stream's resolution in Remote Displays on the Frame.
- An HDR monitor's HDR is off while it streams, and back on after. An HDR desktop in the stream looked washed out.
- A virtual display that goes away leaves your monitor layout as it was.
- The Web UI doesn't stall after a client disconnects.

## Stream over a Steam Link dongle

A Steam Link dongle in the PC connects straight to the Frame's hotspot, so the streams skip your router. On a busy home network that loses far fewer frames. Set the dongle up in Steam as you would for streaming VR games from the PC. In Remote Displays, the computer's Connection setting picks auto (the dongle when it's up), network only, or dongle only, and Find looks for the PC on the hotspot.

## Good to know

- The Frame signs in once. From Vibepollo it keeps a token that can only pair its displays, set their permissions and list the monitors, not the password. You can revoke the token in the Web UI under API Tokens.
- The Web UI is at https://localhost:47990 on the PC.
- The Steam Frame can't reach a PC that's asleep. Set the PC not to sleep while you use it.
- The setup's log is `%ProgramData%\Frametop\host-setup.log`, and its backups of Vibepollo's settings are in `%ProgramData%\Frametop\backup-*`.

## Check and remove

Run these in a Command Prompt in the kit's folder:

- `"Setup Frametop host.cmd" -Check` shows what the setup would change, without changing anything.
- `"Setup Frametop host.cmd" -Undo` puts back the original `sunshine.exe` and the settings from before the setup. The Web UI login stays, and so does Vibepollo. Uninstall Vibepollo from Windows Settings > Apps if you want it gone.
- `"Setup Frametop host.cmd" -SkipLogin` sets up everything except the Web UI login.

## Source

This kit is `host/windows` in [Frametop](https://github.com/Frametop/frametop), under the MIT license. Vibepollo is [Nonary's](https://github.com/Nonary/Vibepollo), and Frametop's build is [Frametop/frametop-vibepollo](https://github.com/Frametop/frametop-vibepollo), both under GPL-3.0. The setup downloads Vibepollo from its GitHub release and Frametop's build from the fork's release.
