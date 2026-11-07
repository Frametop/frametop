# Shared clipboard: copied text between the desktop and a remote display's computer

Status: built 2026-11-07 (DEE-218), text only, for Windows computers that stream with Vibepollo. Not yet tried in the headset. Dragging files between them isn't built.

Turn on Share the clipboard on a computer's card in Frametop Remote Displays (`ft-layout remote host NAME clipboard=on|off`, `"clipboard": true` on its layout entry; off by default). Then:

- Copy text on the Frametop desktop, click one of the computer's displays, and paste it there.
- Copy text on the computer, click back on the desktop (or a floating window), and paste it there.

It's one switch per computer, so a computer that shares its clipboard some other way (KDE Connect, for one) can leave it off.

## How it works

Nothing new runs on the computer. Vibepollo already has Apollo's clipboard request: `GET` and `POST https://HOST:47984/actions/clipboard?type=text` on its GameStream HTTPS port. It takes them only from a paired client that is streaming at that moment, with the client permissions Clipboard Set and Clipboard Read. Its Windows version reads and writes the Windows clipboard's text (Unicode, CRLF line ends). Its Linux version answers but has no clipboard yet. Sunshine, and so the Mac, has no such request.

On the Frame, the syncs happen at the two moments typing changes sides, as ft-screens already tells each remote display's ft-stream:

| When | ft-screens says | ft-stream does |
|---|---|---|
| A click on the display, or a spin that brings it to the front | `focus` (on every click; it acts once until a `blur`) | Sends the desktop's text to the computer, if it changed since the last sync |
| A click on the desktop, another display, frunner opening, or Release all keys | `blur` | Brings the computer's text to the desktop, if it differs from the desktop's |

A key pressed on the display after a `blur` counts as a `focus` too: typing can come back to a display without a click.

- **The desktop's side** is `ft-clip` (`stream/ft-clip.c`), next to ft-stream, a Wayland client of the Frametop desktop's KWin (`$XDG_RUNTIME_DIR/frametop/wayland-0`). It uses wlr-data-control (`stream/protocols/wlr-data-control-unstable-v1.xml`), which KWin 6.2.5 gives clients that don't have focus, as a clipboard manager needs. `ft-clip get` prints the clipboard's text. `ft-clip set` takes it from stdin, then leaves a child that hands it to apps that paste it until something else is copied, as `wl-copy` does. Without the desktop (`desktops.sh streams`) there's nothing to share, and ft-stream says so once in its log.
- **The computer's side** is `Client::Clipboard` in `stream/host.cpp`: the display client's own certificate, and the Web UI certificate's public key, which Remote Displays saved when it signed in (Vibepollo serves the same certificate on both ports). The pin is required: without a saved one (`hosts/HOST.pin`) the clipboard isn't shared with that computer, since anyone in between could read the text or put a command in it for a terminal to paste. It opens a connection for each request and closes it after, as every other request ft-stream makes does.
- **The rules** are in `stream/clipboard.h`: text up to 1 MiB each way, never empty, CRLF and LF counted as the same text, and the desktop gets LF. They run on a thread of their own in ft-stream, in order, so the stream never waits for them. Without `XDG_RUNTIME_DIR` nothing is shared (no fallback to a shared `/tmp`).
- **Passwords stay put.** A copy offered with `x-kde-passwordManagerHint` (KeePassXC marks its passwords so for clipboard managers) isn't sent: on Windows it could end up in Clipboard History or cloud sync.
- **A busy clipboard.** Vibepollo answers 500 when another Windows app has the clipboard open at that moment; the next sync tries again. A paste that stops reading gets 2 s, then ft-clip moves on to the next one.

### Not losing what wasn't text

The computer can hold something that isn't text: an image, cells, files. Vibepollo reads that as no text, and the desktop keeps what it has. Coming back to the computer must not then replace that with the desktop's older text. So each computer has a small state file, `$XDG_RUNTIME_DIR/frametop-clipboard-ID` (0600, shared by its displays' ft-streams): a hash of the text at the last sync. The desktop's text goes to the computer only when it changed since then.

The file holds a hash, never the text, and ft-stream logs only sizes.

## Permissions

New displays are paired with Clipboard Set and Clipboard Read, besides launching, mouse and keyboard (`kFrametopPerm`). The text moves only while the switch is on: the permission alone sends nothing. A display paired before this needs them added: in Vibepollo's Web UI, Clients, tick both Clipboard permissions for its client, or remove the display and add it again. Until then the computer answers 401, and ft-stream logs once that the display may not use the clipboard, and stops asking.

## Not built

- Images, rich text and files. Vibepollo's request is text only (`type=text`), and so is ft-clip.
- Dragging files between the desktop and the computer. Vibepollo has file upload and download permissions, but no requests for them yet.
- Linux computers (Vibepollo's `get_clipboard` and `set_clipboard` are placeholders there) and Macs (Sunshine has no clipboard request).
- Seeing a copy on the computer while a display has typing: the text comes over when typing leaves it, not as it's copied.
- Copying on one computer and pasting on another in one click: each display's ft-stream syncs on its own, so clicking computer B right after copying on computer A can send B the desktop's older text before A's arrives. Click the desktop in between. Two displays of the same computer share its state file, so this case is narrow there.
