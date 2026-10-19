# Frametop host for Linux

A Linux PC can be a host for Frametop's remote displays with Frametop's build of Vibepollo, from [Frametop/frametop-vibepollo](https://github.com/Frametop/frametop-vibepollo). Vibepollo's Linux version is in beta, and Frametop hasn't been tested on a Linux host yet. If you try it, please report what works and what doesn't, on [GitHub](https://github.com/Frametop/frametop/issues) or in the [Frametop Discord](https://discord.gg/W3X9f7z3Bc).

There's no setup script for Linux. These steps use Vibepollo's own installer with Frametop's package.

## Requirements

These are Vibepollo's:

- Arch Linux or CachyOS, x86_64. Other distributions have no package.
- KDE Plasma 6 on Wayland, started by SDDM or Plasma Login Manager. GNOME, X11 sessions and other compositors aren't streamed.
- Linux 6.16 or newer, with the headers for the running kernel. The installer installs them if they're missing.
- A graphics card with a hardware H.264 encoder: NVENC on NVIDIA, VAAPI on AMD and Intel.
- The PC and the Steam Frame on the same network.

## Set up

1. Download Frametop's package and Vibepollo's installer, and check the package:

   ```bash
   mkdir -p ~/frametop-host && cd ~/frametop-host
   release=https://github.com/Frametop/frametop-vibepollo/releases/download/frametop-2.0.0-3
   curl -fLO "$release/vibepollo-2.0.0-1-x86_64.pkg.tar.zst"
   curl -fLO "$release/vibepollo-2.0.0-1-x86_64.pkg.tar.zst.sha256"
   sha256sum -c vibepollo-2.0.0-1-x86_64.pkg.tar.zst.sha256
   curl -fLO https://raw.githubusercontent.com/Frametop/frametop-vibepollo/frametop-2.0.0-3/scripts/linux_install.sh
   ```

   `sha256sum -c` should print `OK`.
2. Install it:

   ```bash
   sudo bash linux_install.sh --package ./vibepollo-2.0.0-1-x86_64.pkg.tar.zst
   ```

   The installer checks the requirements, installs the kernel headers if needed, installs the package, and opens the firewall (firewalld or ufw). Given a package, it doesn't add Vibepollo's own package repository. Don't add that repository later: an update from it would replace Frametop's build.
3. Restart the PC after the first install, and log in to Plasma (Wayland).
4. Open https://localhost:47990 on the PC and create the Web UI login. The Steam Frame signs in with it once.
5. If you'll use virtual displays, turn on "Remove Remote Monitor when its client disconnects" in the Web UI's settings. Then a virtual display goes away when Frametop disconnects it, as on Windows.
6. On the Steam Frame, open Frametop Remote Displays and choose Add computer. Pick this PC, sign in with the Web UI login, and choose the displays to show.

If the installer couldn't open the firewall, open it yourself:

```bash
sudo firewall-cmd --permanent --add-service=vibepollo && sudo firewall-cmd --reload   # firewalld
sudo ufw allow Vibepollo                                                                # ufw
```

## Good to know

- The resolution switch, the HDR guard and the Frame's microphone are Windows-only for now. On Linux, a monitor keeps its own resolution and the stream scales it, and Send my microphone does nothing.
- Vibepollo runs as system services. `sudo systemctl status vibepollo-session-controller.service vibepollo.service` shows them, and `sudo journalctl -u vibepollo-session-controller.service -u vibepollo.service --since '-10 minutes'` shows their log. Don't start `vibepollo.service` yourself: the session controller starts it when you log in.
- Settings, the Web UI login and pairings are in `/var/lib/vibepollo`.
- Vibepollo's [Linux install guide](https://github.com/Frametop/frametop-vibepollo/blob/frametop-2.0.0-3/docs/linux/install.md) has the details and troubleshooting.

## Remove

```bash
sudo vibepollo reset      # only to delete its settings, login and pairings too
sudo pacman -R vibepollo
```

## Source

Vibepollo is [Nonary's](https://github.com/Nonary/Vibepollo), and Frametop's build is [Frametop/frametop-vibepollo](https://github.com/Frametop/frametop-vibepollo), both under GPL-3.0.
