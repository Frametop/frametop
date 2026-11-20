# Frametop host for Windows

This kit makes a Windows PC a host for Frametop's remote displays: the PC's monitors appear as screens on a Steam Frame, and it can also make virtual monitors at any size. It can also show single app windows of the PC as panels, lend the PC's keyboard and mouse to the Frame, and share the clipboard. The PC streams with Vibepollo, a version of Sunshine, and Frametop's build of Vibepollo.

## Requirements

- Windows 10 or 11, 64-bit.
- A graphics card with a hardware video encoder. It's tested on an NVIDIA card.
- An admin account. The setup asks Windows for admin, so you confirm one prompt.
- The PC and the Steam Frame on the same network, or a Steam Link dongle plugged into the PC (see [Stream over a Steam Link dongle](#stream-over-a-steam-link-dongle)).
- Frametop on the Steam Frame.

If Sunshine runs on this PC, uninstall it first: Vibepollo uses the same ports. If Apollo is installed, Vibepollo's installer replaces it and keeps its settings and pairings.

## Set up

The quickest way: download [FrametopHostSetup.exe](https://github.com/Frametop/frametop-vibepollo/releases/download/frametop-2.0.4/FrametopHostSetup.exe) from Frametop's fork of Vibepollo and run it. It isn't signed, so Windows may say "Windows protected your PC": choose More info, then Run anyway. It holds no versions itself: it runs the PowerShell line below, so it always installs the build this kit pins, and you can keep it to update later. Run it from a Command Prompt with `-Check`, `-Undo` or `-SkipLogin` for the options under [Check and remove](#check-and-remove). The kit downloads from GitHub: from [Frametop/frametop](https://github.com/Frametop/frametop) (`main`), from [Frametop/frametop-vibepollo](https://github.com/Frametop/frametop-vibepollo/releases/tag/frametop-2.0.4) (Frametop's build, release `frametop-2.0.4`), and Vibepollo's installer from [Nonary/Vibepollo](https://github.com/Nonary/Vibepollo) when it isn't installed (Frametop's build goes into it: Vibepollo's installer brings the drivers, which only upstream can sign).

Or open PowerShell on the PC (Start, type `powershell`, Enter; no admin needed) and run

```powershell
irm https://raw.githubusercontent.com/Frametop/frametop/main/host/windows/get.ps1 | iex
```

It downloads this kit's setup into `%LOCALAPPDATA%\Frametop\host-kit` and runs it, as in step 3 below: an admin window opens and the setup goes on there. Run the same line again for a newer kit. For the options under [Check and remove](#check-and-remove), run `"Setup Frametop host.cmd"` in that folder with them, or pass them to the one line: `& ([scriptblock]::Create((irm https://raw.githubusercontent.com/Frametop/frametop/main/host/windows/get.ps1))) -Check`.

Or with the kit's zip:

1. Download `frametop-windows-host-1.zip` from the [`windows-host-1`](https://github.com/Frametop/frametop/releases/tag/windows-host-1) release and extract it. You get a folder named `frametop-windows-host`.
2. Double-click `Setup Frametop host.cmd` in that folder. Windows may warn that the file came from the internet: choose Run. Then allow the admin prompt.
3. The setup does the following:
   - If Vibepollo isn't installed, it downloads Vibepollo 2.0.3, checks the download's SHA-256 and runs Vibepollo's installer. Follow the installer's steps, then go back to the setup window. It does the same over an earlier kit's setup (Vibepollo 2.0.0), since Frametop's build now goes into 2.0.3.
   - It puts in Frametop's build, after checking each file's SHA-256: `sunshine.exe`, and in Vibepollo's `tools` folder `sunshine_wgc_capture.exe` (it captures app windows and their menus) and `frametop_clipboard.exe` (it reads the clipboard). Vibepollo's own files are kept next to them as `<name>.2.0.3-original`.
   - It changes three Vibepollo settings: remote displays carry the PC's sound, and a virtual display goes away when Frametop disconnects it but stays through a dropped stream. Your other settings stay as they are.
   - It asks for the Web UI user name and password that the Steam Frame signs in with. If Vibepollo already has a login, you can keep it. The password is typed in the setup window and is never shown or saved.
   - It lets Vibepollo through the firewall on every network type, says whether a Steam Link dongle is plugged in, and checks that the Web UI answers.
4. When it says Done, it prints this PC's name and addresses. On the Steam Frame, open Remote displays in Frametop Settings and choose Add computer. Pick this PC, sign in with the Web UI login, and choose the displays to show: its monitors, or a virtual display at the size you pick.

When a newer kit comes out, run its setup the same way. It replaces an older Frametop build.

Monitors you plug in later need nothing on the PC: Add displays asks Vibepollo for the PC's monitors each time it opens. A virtual monitor is made when its stream starts and goes away when you disconnect or remove it in Remote Displays. It stays through a dropped stream, so its windows keep their places. At most four virtual monitors exist at once.

## What Frametop's build changes

Frametop's build goes into Vibepollo 2.0.3 and changes these things. It comes from [Frametop/frametop-vibepollo](https://github.com/Frametop/frametop-vibepollo):

- It can stream any of the PC's monitors, not only the main one, and several at once.
- While a monitor streams, it switches to the stream's resolution (or the closest mode it has with the same shape), and back when the stream ends. Set the stream's resolution in Remote Displays on the Frame.
- An HDR monitor's HDR is off while it streams, and back on after. An HDR desktop in the stream looked washed out.
- A virtual display that goes away leaves your monitor layout as it was.
- The Web UI doesn't stall after a client disconnects.
- It can take the Frame's microphone, for calls and voice chat on the PC. Turn on Send my microphone on the PC's card in Remote Displays. While its displays stream, apps on the PC can record it as "Microphone (Steam Streaming Microphone)". That's Steam's virtual microphone, so the PC needs Steam installed: the build installs the microphone from Steam's drivers the first time if it's missing, and leaves your default microphone as it was. Without Steam the PC gets nothing, and Vibepollo's log says so. It only goes over the encrypted stream.
- App windows: Frametop's launcher lists the PC's windows and installed apps, Store apps too, and opens each one as a panel of its own. Windows on panels move to a virtual display of their own and go back where they were when released. The tray's **Bring windows back** puts them all back. App icons don't show yet.
- The Frame can borrow the PC's keyboard and mouse when you ask in the headset. It's on by default; set `frametop_host_input = disabled` in `sunshine.conf` to turn it off. Both Shift keys + Esc on the PC take them back at once.
- With Share the clipboard on for the PC in Remote Displays, text and PNG images go both ways.

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
- `"Setup Frametop host.cmd" -Undo` puts back Vibepollo's own files (removing `frametop_clipboard.exe`, which Vibepollo doesn't have) and the settings from before the setup. The Web UI login stays, and so does Vibepollo. Uninstall Vibepollo from Windows Settings > Apps if you want it gone.
- `"Setup Frametop host.cmd" -SkipLogin` sets up everything except the Web UI login.

## Source

This kit is `host/windows` in [Frametop](https://github.com/Frametop/frametop), under the MIT license. Vibepollo is [Nonary's](https://github.com/Nonary/Vibepollo), and Frametop's build is [Frametop/frametop-vibepollo](https://github.com/Frametop/frametop-vibepollo), both under GPL-3.0. The setup downloads Vibepollo from its GitHub release and Frametop's build from the fork's release.
