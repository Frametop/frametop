# Shared clipboard: copied text and images between the desktop and a remote display's computer

Status: text built 2026-11-07 (DEE-218) for Windows computers. Linux computers and PNG images both ways added 2026-11-08, which need Frametop's build of Vibepollo from then. Not yet tried in the headset. Dragging files between them isn't built.

Turn on Share the clipboard on a computer's card on Frametop Settings' Remote displays page (`ft-layout remote host NAME clipboard=on|off`, `"clipboard": true` on its layout entry; off by default). Then:

- Copy text or an image on the Frametop desktop, click one of the computer's displays, and paste it there.
- Copy text or an image on the computer, click back on the desktop (or a floating window), and paste it there.

It's one switch per computer, so a computer that shares its clipboard some other way (KDE Connect, for one) can leave it off.

## How it works

Nothing new runs on the computer. Vibepollo has Apollo's clipboard request: `GET` and `POST https://HOST:47984/actions/clipboard?type=text` on its GameStream HTTPS port. It takes them only from a paired client that is streaming at that moment, with the client permissions Clipboard Set and Clipboard Read. Frametop's Vibepollo adds `type=image` (a PNG file's bytes, `image/png`; an empty answer when the clipboard holds no image) and the Linux clipboard:

- **Windows** reads and writes the Windows clipboard's text (Unicode, CRLF line ends) and its images: a copied image comes from the registered `PNG` format if an app put one there (browsers, Office, the Snipping Tool: transparency kept), else from the bitmap (`CF_DIB`, made a PNG with WIC, without alpha). Vibepollo, started by its service, can't see the formats other apps register, so a small reader, `tools\frametop_clipboard.exe`, reads the clipboard as the signed-in user for each request (about 50 ms); without it nothing is read. An image from the Frame goes on as both `PNG` and a 32-bit bitmap, so apps that know only bitmaps can paste it.
- **Linux** uses the desktop session's clipboard through `vibepollo-clipboard`, a Wayland data-control client that Vibepollo starts the first time it's asked (by its session broker, as the desktop user, when Vibepollo runs as the system service). That works on KDE Plasma, Hyprland and the wlroots desktops; GNOME has no data-control protocol, so there the computer answers 500 to a copy and has nothing to read. See `linux/hyprland.md` in the fork's docs folder.
- Vibepollo builds without `type=image` (Apollo, upstream Vibepollo, Frametop's before 2026-11-08) answer 400 to an image: that display's ft-stream then shares text only. Sunshine, and so the Mac, has no clipboard request at all.

On the Frame, the syncs happen at the two moments typing changes sides, as ft-screens already tells each remote display's ft-stream:

| When | ft-screens says | ft-stream does |
|---|---|---|
| A click on the display, or a spin that brings it to the front | `focus` (on every click; it acts once until a `blur`) | Sends the desktop's clipboard to the computer, if it changed since the last sync |
| A click on the desktop, another display, frunner opening, or Release all keys | `blur` | Brings the computer's to the desktop, if it changed since the last sync and differs from the desktop's |

A key pressed on the display after a `blur` counts as a `focus` too: typing can come back to a display without a click.

**Text wins.** A clipboard that holds text and an image (cells copied from a spreadsheet, say) shares its text. ft-stream reads a side's text first and asks for its image only when it has no text.

- **The desktop's side** is `ft-clip` (`stream/ft-clip.c`), next to ft-stream, a Wayland client of the Frametop desktop's KWin (`$XDG_RUNTIME_DIR/frametop/wayland-0`). It uses wlr-data-control (`stream/protocols/wlr-data-control-unstable-v1.xml`), which KWin 6.2.5 gives clients that don't have focus, as a clipboard manager needs. `ft-clip get` prints the clipboard's text and `ft-clip get-image` its `image/png`. `ft-clip set` and `set-image` take one from stdin, then leave a child that hands it to apps that paste it until something else is copied, as `wl-copy` does. Without the desktop (`desktops.sh streams`) there's nothing to share, and ft-stream says so once in its log.
- **The computer's side** is `Client::Clipboard` in `stream/host.cpp`: the display client's own certificate, and the Web UI certificate's public key, which Remote Displays saved when it signed in (Vibepollo serves the same certificate on both ports). The pin is required: without a saved one (`hosts/HOST.pin`) the clipboard isn't shared with that computer, since anyone in between could read it or put a command in it for a terminal to paste. It opens a connection for each request and closes it after, as every other request ft-stream makes does.
- **The rules** are in `stream/clipboard.h`: text up to 1 MiB and PNG images up to 32 MiB each way, never empty, CRLF and LF counted as the same text, and the desktop gets LF. An image must be a PNG (its file signature is checked on both sides). They run on a thread of their own in ft-stream, in order, so the stream never waits for them. Without `XDG_RUNTIME_DIR` nothing is shared (no fallback to a shared `/tmp`).
- **Passwords stay put.** A copy offered with `x-kde-passwordManagerHint` (KeePassXC marks its passwords so for clipboard managers) isn't sent, from the desktop or from a Linux computer: on Windows it could end up in Clipboard History or cloud sync. On Windows, a copy marked `ExcludeClipboardContentFromMonitorProcessing` (password managers there) isn't read either (KeePass, and KeePassXC and other apps that copy through OLE: tried both ways).
- **A busy clipboard.** Vibepollo answers 500 when another Windows app has the clipboard open at that moment; the next sync tries again. A paste that stops reading gets 2 s (10 s for an image), then ft-clip moves on to the next one.

### Not losing what can't come over

The computer can hold something that can't come over: files, an image too big, or any image on a text-only host. Vibepollo reads that as nothing, and the desktop keeps what it has. Coming back to the computer must not then replace that with the desktop's older clipboard. So each computer has a small state file, `$XDG_RUNTIME_DIR/frametop-clipboard-ID` (0600, shared by its displays' ft-streams): a hash of what was shared at the last sync (an image's hash differs from any text's). The desktop's goes to the computer only when it changed since then, and the computer's comes to the desktop only when it changed since then: an image the desktop holds that a text-only computer couldn't take, or something copied on the desktop while a display had typing, isn't replaced by the computer's older text.

The file holds a hash, never the content, and ft-stream logs only sizes.

## Permissions

New displays are paired with Clipboard Set and Clipboard Read, besides launching, mouse and keyboard (`kFrametopPerm`). Nothing moves unless the switch is on: the permission alone sends nothing. A display paired before this needs them added: in Vibepollo's Web UI, Clients, tick both Clipboard permissions for its client, or remove the display and add it again. Until then the computer answers 401, and ft-stream logs once that the display may not use the clipboard, and stops asking.

## Not built

- Rich text, other image formats (an app that offers only JPEG or BMP on Linux), and files.
- Dragging files between the desktop and the computer. Vibepollo has file upload and download permissions, but no requests for them yet.
- Macs (Sunshine has no clipboard request).
- Seeing a copy on the computer while a display has typing: it comes over when typing leaves the display, not as it's copied.
- A changed image on the computer is read again at each `blur` (there's no cheap "has it changed" request), so a big one costs its size in Wi-Fi each time typing leaves its display.
- Copying on one computer and pasting on another in one click: each display's ft-stream syncs on its own, so clicking computer B right after copying on computer A can send B the desktop's older clipboard before A's arrives. Click the desktop in between. Two displays of the same computer share its state file, so this case is narrow there.
