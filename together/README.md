# Frametop Together

Shared rooms: up to four people in one owner's room, each on their own Frame, over Tailscale. The design, its decisions and the milestones are in [docs/together.md](../docs/together.md). This file says what is in this folder and what goes over a link.

Built so far: the spikes (M0), and from M3 the session's wire format and rules, the links, `ft-together`, which opens a room, lets people in and keeps the room's people linked, and poses: each person's head and hands go to the others, and `ft-avatars --room` ([avatar/README.md](../avatar/README.md)) draws them, in the avatar they sent or as abstract figures. Shared panels: the room's list of them, the video's path through `ft-together` from the program that encodes a panel to the programs that show it, and its rules are done and tested. The programs at each end (`ft-sharepanel`, `ft-peerpanel`, ft-screens' share tap and peer screens, `ft-roompanels`) are written and tested without video; the video through the hardware encoder and decoder has not run yet ("A shared panel on a Frame" below says why). Voice is not built yet.

## What is where

| Folder | What it holds | Depends on |
|---|---|---|
| `proto/` | The bytes on a link: bounded readers and writers (`bytes.h`), the control stream's messages (`messages.h`, with the avatar a person has on as the room lists it), a shared panel as the room lists it (`panel.h`), a picture on its stream (`framewire.h`), the pose datagram (`posewire.h`), the voice datagram (`voicewire.h`). No I/O, standard library only, so the PC sender in the Vibepollo fork can take the folder as it is | nothing |
| `room/` | A room's own rules: invites (`invites.h`), where people stand (`seats.h`), whose panels are in it (`panels.h`), and how often someone may do a thing (`allowance.h`) | `proto/` |
| `session/` | The session: who may join, who is in the room, who links to whom, and what each link may say (`session.h`; `session.cpp` has what the links bring, `wearer.cpp` what this Frame's wearer does). The network, the tailnet, the clock and the dice are handed in. `commands.h` puts a session behind lines of text | `proto/`, `room/` |
| `flow/` | The shared panels' video, its rules: who gets a panel's next picture (`sendgate.h`), the order a viewer decodes them in (`framegate.h`), the bitrate (`pace.h`), and the path through ft-together between the links and this Frame's media programs (`video.h`). And the avatars people have on, their rules (`looks.h`): who is asked for a file, who is sent one, and whose is kept. The links, the programs and the files are handed in | `proto/`, `room/`, `session/`, OpenSSL (SHA-256) |
| `local/` | What ft-together does for this Frame's own programs beyond the session's commands: the poses and voices it lets through from a link (`posegate.h`, `voicegate.h`, both on the count and the allowance of `linkgate.h`), who gets the others' poses and voices (`watchers.h`), where this Frame's floor lies in the room (`floor.h`), the datagrams on the local socket that aren't text (`datagrams.h`), the media programs on their own socket (`media.h`, with what is said there in `mediawire.h`), and the avatars' files on this Frame: the wearer's own and the ones that came (`wardrobe.h`) | `proto/`, `flow/`, `avatar/modelfile.h` |
| `net/` | The real network: the links on msquic (`quiclinks.h`; `quiclinks.cpp` has the connections, the control stream and the datagrams, `quicframes.cpp` the streams that carry a picture each, `quicfiles.cpp` the ones that carry an avatar's file), with the self-signed certificate QUIC wants (`selfcert.h`), which address a name stands for (`lookup.h`), and who is at an address by tailscaled's word (`tailnet.h`, with the small JSON reader for its answers, `json.h`) | `session/`, msquic, OpenSSL |
| `ft-together.cpp` | The program: one loop over the links and the local sockets | `session/`, `flow/`, `net/`, `local/` |
| `ft-room` | Asks a running ft-together one question from a shell: `together/ft-room state`, `together/ft-room open`, `together/ft-room --socket NAME invite` | `conf/ftipc.py` |
| `media/` | The iris hardware encoder (`irisenc.h`), and what `ft-sharepanel` is made of: the encoder's input buffers (`pictures.h`), the GPU pass that draws a screen's picture into one (`blit.h`), the encoder for one panel (`panelenc.h`), and where pictures come from (`feed.h`): a screen of ft-screens' (`screenfeed.h`) or a test picture (`patternfeed.h`, `testpattern.h`). `words.h` reads the lines the programs trade with ft-screens | V4L2, GBM, EGL |
| `ft-sharepanel.cpp` | Encodes one screen this Frame shares | `media/`, `local/mediaclient.h` |
| `ft-peerpanel.cpp` | Decodes one panel someone else shares, into a remote screen of ft-screens' | `stream/iris.h` (the remote displays' decoder), `local/mediaclient.h` |
| `roompanels/` | ft-roompanels' rules: the sums between this Frame's floor and the room (`placing.py`) and what it keeps true (`keeper.py`) | Python's standard library |
| `ft-roompanels` | The program around them: its loop and its socket | `roompanels/`, `conf/ftipc.py` |
| `roomaway/` | ft-roomaway's rules: what the pointer helper and the relay say of the wearer, and what the room is told (`teller.py`) | Python's standard library |
| `ft-roomaway` | The program around them: tells the room when the headset comes off or a game starts | `roomaway/`, `conf/ftipc.py` |
| `roompage.py` | Frametop Settings' Together page without Qt: the programs' answers read into what the page says, and what is typed read into a `join` | Python's standard library |
| `ft_together_settings.py`, `TogetherPages.qml` | The page: its backend, which asks the room's programs off the UI's thread, and its QML | `roompage.py`, `conf/ftipc.py`, `conf/fthost.py`, PySide6 |
| `frametop-together.service`, `frametop-room-*.service`, `run.sh` | The user units the programs run as, and their installer | `scripts/install-lib.sh` |
| `voice/` | What `ft-roomvoice` is made of: Opus each way (`codec.h`), the buffer that plays a voice's packets in step however they arrive (`jitter.h`), where a voice is heard from (`spatial.h`), everyone's voices mixed for two ears (`voices.h`), the thread that sends the wearer's (`sender.h`) and the one that plays the others' (`player.h`), the sound devices behind two small interfaces (`sound.h`, on PulseAudio in `pulse.cpp`, pretended for tests in `pretend.h`), and where the wearer's head is (`headask.h`) | libopus, libpulse-simple |
| `ft-roomvoice.cpp` | The room's voices on this Frame | `voice/`, `avatar/room/client.h` |
| `spike/` | The M0 spikes, `ft-enctest` and `ft-quictest` | `media/`, `net/` |
| `tests/` | The tests below | |

`together/build.sh` builds `build/ft-together`, `build/ft-sharepanel`, `build/ft-peerpanel`, `build/ft-roomvoice` and msquic (pinned by commit) in the dev container, and with `--spikes` the spikes too.

## Running it

Frametop Together is a part of its own: `install.sh` offers it (no by default while it is experimental; `--together` and `--no-together` answer for it), and `together/run.sh install` installs it later. Installed, it is off. The switch on Frametop Settings' Frametop Together page turns it on, now and at every login, as `together/run.sh on` does.

| Unit | Runs | Where |
| --- | --- | --- |
| `frametop-together.service` | `ft-together` | the container |
| `frametop-room-people.service` | `ft-avatars --room`: the others, drawn, and your own pose told | the container; it waits for SteamVR |
| `frametop-room-panels.service` | `ft-roompanels` | the host |
| `frametop-room-voice.service` | `ft-roomvoice` | the container |
| `frametop-room-away.service` | `ft-roomaway` | the host |

Only the first is ever enabled. It wants the other four, and each of them is part of it, so they start, stop and restart with it and never run without it. Outside a room they wait: none of them opens the microphone, draws, or encodes anything until ft-together says this Frame is in one. `together/run.sh status` shows the five, `log` their journal, and `off` stops them and leaves a room this Frame is in (its own room ends for everyone). ft-together takes calls only on the tailnet address, and answers nobody who isn't on the tailnet (below, "The links").

`scripts/fast-install` builds `together/` as a component, and a release brings it built (`pack/Containerfile`); msquic is fetched at the commit `together/build.sh` pins.

### The page

Frametop Settings > Frametop Together (`ft_together_settings.py`, `TogetherPages.qml`) is how a room is used from inside the headset:

- The switch, with what ft-together says when it can't run ("Tailscale isn't running on this headset"), and this headset's tailnet address to give to a guest.
- In no room: Open a room, or join someone's with their headset's address (`name`, `name:port` or `name port`) and the invite code they gave you.
- In your own room: Make an invite code (shown until the room ends, with a copy button), and each person who asks to come in with Let in and No. Closing the room asks first when others are in it.
- In any room: who is in it and how (you, the owner, in a game, headset off, connecting), and Leave.
- What you share: your voice (the switch mutes `ft-roomvoice`), and a switch per screen of the desktop (`share N` and `unshare N` to `ft-roompanels`), three at most.

The page asks `state` and `here` of ft-together, `state` of ft-roomvoice and ft-roompanels and `screens` of ft-screens once a second while it shows, on a worker thread, and nothing while it doesn't. Names, logins and notes in the answers are text a peer chose: they are shown as plain text, and what is typed for a `join` is sent only when it reads as an address and a code (`roompage.py`).

Not there yet: someone asking to come in shows only on this page, not as a notification; mute is forgotten when ft-roomvoice restarts.

## ft-together

```
ft-together [--port N] [--name NAME] [--socket NAME] [--loopback]
```

It runs while Together is wanted. It asks tailscaled for this Frame's tailnet address and name, takes calls on that address (port 47193 unless `--port` says otherwise), and holds the session. While Tailscale isn't running it waits and looks again every three seconds. `--name` is what the others call you; without it, the headset's name on the tailnet.

The rest of Frametop talks to it on `@ft_together`, one datagram a question and one back, like the other Frametop sockets:

| Question | Answer |
|---|---|
| `state` | `ok` and the session as JSON: `mode` (`idle`, `hosting`, `calling`, `joined`), `note` (the last thing worth telling the wearer), `me`, `away` (how this Frame's own wearer is away: 0, 1 in a game, 2 with the headset off), `owner`, `people` (place, name, login, away, whether there is a link to them, spot), `panels` (each shared panel's id, owner, number, name, size in pixels, place and turn in the room, width in metres and curve), `asks` (who waits for the owner's answer: number, name, Tailscale login, address) and `codes` (invites nobody used yet) |
| `open [X Z YAW]` | A room, with you its owner standing there |
| `invite` | `ok` and a code |
| `join HOST PORT CODE` | Calls a room's owner. `state` tells how it went |
| `accept N`, `decline N` | An ask answered |
| `away game`, `away off`, `away back` | How this Frame's wearer is away. In a room the room is told. Outside one it is kept, and the next room has it from the start |
| `leave` | Leaves the room. Its owner leaving ends it for everyone |
| `share K W H X Y Z QW QX QY QZ METRES CURVE [NAME]` | Shares your panel number K (0 to 2) with the room, or says where it is now: its video's size in pixels, its middle and its turn in the room (the panel faces its own +z), its width in metres, and its curve (0 for flat, else the radius in metres). Saying the same again costs nothing |
| `unshare K` | Takes it back |
| `wear NAME`, `wear` | The avatar you have on is the model NAME in your avatars folder, or none. The program that draws you says it, as often as it likes: the file is read again only when it changed. `ok` when the room may hear of it; `error` and why when the others will see the abstract figure instead ("The avatars people have on") |
| `looks` | `ok` and, as JSON: `wear` (the model you have on), `sent` (whether the others may have it), `why` (why not), and `people`, a row for each of the others who has an avatar on: their place (`id`) and where its file is on this Frame (`file`, empty until it came) |
| `here` | Where the others call this Frame: `ok ADDRESS PORT NAME`, its tailnet address, the port it takes calls on, and what the others call you. An invite is that address and port and a code from `invite` |
| `media` | `ok` and how the shared panels' video goes, as JSON: which of your panels have a program that encodes them (`sources`), which panels have a program that shows them here (`sinks`), and for each of your three panels the bitrate its encoder is asked for and how many people watch it |
| `watch` | `ok`, and for the next five seconds every pose a person in the room sends comes to the asker (below). A program that draws the room asks again each second. At most four watch at once |
| `listen` | `ok`, and for the next five seconds every voice datagram a person in the room sends comes to the asker (below). Asked again each second, like `watch`. At most four listen at once |
| `floor` | Where this Frame's own floor lies in the room: `ok X Z YAW`, or `ok none` while no program has said |
| `floor X Z YAW` | Says where it lies. The first said stands until you leave the room, and the answer is the one that stands |

Four kinds of datagram on the socket aren't text, and are told by their first byte, which no text starts with:

- **Your own pose**, to ft-together: a pose datagram as it goes on a link (first byte 1, below), in the room's directions. ft-together reads it as it reads a peer's, puts its own count in it and sends it to everyone in the room it has a link to. It isn't answered, and outside a room it goes nowhere.
- **Someone's pose**, to each watcher: the byte `0x81`, their place in the room (a byte), then their pose datagram. Only a pose from a link whose person is in the room is passed on, only if it reads, is newer than that link's last, and is within 90 datagrams a second from that link (three times what a Frame sends), rubbish counted.
- **Your own voice**, to ft-together: a voice datagram (first byte 2, below). It is sent as it is to everyone in the room ft-together has a link to. It isn't answered, and outside a room it goes nowhere.
- **Someone's voice**, to each listener: the byte `0x82`, their place in the room, then their voice datagram. Only one from a link whose person is in the room is passed on, only if it reads, is newer than that link's last, and is within 150 a second from that link (three times what a Frame sends). ft-together doesn't look into the Opus packet.

**Your floor in the room.** Everyone tracks themselves on their own floor, and the room has its owner's directions. A place on your floor, turned by YAW radians about the way up and then moved by X and Z metres, is that place in the room. It is laid once, when you come in, so that where you stand then is the spot the room gave you, facing the way the spot faces. Every program of yours that draws the room or tells it where you are needs the same answer, so ft-together keeps it: the first program that knows where you stood says it, and the rest ask.

Anything that can't be done is answered with `error` and why. Ending the program (SIGTERM) leaves the room first, so the others are told at once.

```
together/ft-room state
echo -n state | socat -t 1 - ABSTRACT-SENDTO:ft_together,bind=
```

### The media programs' socket

The programs that encode your shared panels and show other people's talk to ft-together on a second socket, `@ft_together_media` (`@NAME_media` with `--socket NAME`). It is a stream socket, one connection a program, and only your own user's programs are let in. Every message is a `u32` length (little-endian), a byte that says what it is, and its body ([local/mediawire.h](local/mediawire.h)).

| Who | Message | Meaning |
|---|---|---|
| An encoder, first | `S`, panel number | It encodes your panel of that number |
| An encoder | `U`, flags (1: a keyframe), the encoded picture | One picture, at most 1 MB |
| To an encoder | `R`, bits a second | Encode at this rate from now on |
| To an encoder | `K` | Make the next picture a keyframe |
| To an encoder | `W`, 1 or 0 | Whether anyone watches the panel. If nobody does, nothing need be encoded |
| A viewer's program, first | `V`, panel id | It shows that panel of someone else's |
| A viewer's program | `A`, level (0 hidden, 1 in view, 2 focused), 1 if its decoder needs a keyframe | What the panel has of its viewer |
| To a viewer's program | `U`, flags, the encoded picture | The next picture to decode: in order, none missing since the last keyframe |

One program for each panel: a second one that names the same panel takes the first one's place. A program that says anything else, says nothing for five seconds, or leaves 4 MB waiting for it is let go, and may come again.

`--loopback` is for tests: no Tailscale, the links are on 127.0.0.1 only, and everyone there is let in as `someone@loopback`. A fake peer for the live Frame is a second `ft-together` in the normal mode on another port and socket: it has this Frame's tailnet address, and the session tells people apart by address and port.

## The links

One QUIC connection for every two people, with one stream for the control messages, opened by the caller, datagrams beside it for poses and voice (at most 1187 bytes each, what a link over Tailscale carries), and a one-way stream for each picture of a shared panel. msquic runs threads of its own; what they see is queued, and `ft-together`'s one loop takes it out and tells the session.

- The listener takes calls sent to this Frame's tailnet address only. Who a caller is comes from tailscaled, asked about the address the call came from. The certificate QUIC wants is made new at each start, names nobody, and isn't checked: Tailscale already encrypts the path and vouches for the address at its other end.
- A name is looked up before a call, and only an address on the tailnet is ever sent a packet.
- An end holds at most 32 connections. A link whose bytes pile up untaken past 1 MB is cut, and datagrams past 256 KB untaken are dropped: the session reads faster than anyone has reason to write.
- A picture that isn't all acknowledged 150 ms after it was sent is given up: its stream is reset and the sender's loop is told. At most 64 are on their way out on a link, and more aren't sent. Coming in, a link holds at most 32 picture streams at once and 4 MB in all; a stream past that is refused, which its sender sees as a picture given up.
- An avatar's file goes on a one-way stream too, one at a time on a link and behind everything else on it. Coming in, such a stream is read only when a file was asked for on that link, up to the size asked for, one at a time; any other is refused unread. A file that came waits for the loop outside the 1 MB above.
- Closing a link lets what was sent before arrive, then tells the closer nothing more of it. A link that is silent for ten seconds is down.

## Joining

1. The owner opens a room. They are person 0, standing where they stood when they opened it. The room's directions are the owner's.
2. The owner makes an invite: an eight-character code from letters and digits that read aloud without mix-ups. It works once, for ten minutes. At most eight wait at once.
3. The guest calls the owner's Frame by its tailnet name or address and says hello with the code.
4. The owner's side asks tailscaled who is at the caller's address. A caller tailscaled doesn't know is refused whatever their code. Then the room must have a free place, and then the code must be good. Five wrong codes from one address in ten minutes, and that address is refused without its codes being looked at.
5. The owner sees the guest's name and their Tailscale login, and accepts or declines. An ask nobody answers ends after two minutes.
6. An accepted guest gets the room: its people, their addresses and their spots. The guest's spot is beside the owner, 0.9 m apart: the first to the owner's right, the second to their left, the third further right.

The owner's copy of the room is the room. Guests hold the last one the owner sent, and take a newer one only from the owner's link, only for the same room. The owner leaving, or a guest losing the link to the owner, ends the room for that guest.

## The guests' links

Every two people in a room have one link. A guest calls the owner to join. Of two guests, the one whose address is lower calls the other (address, then port), and calls again every three seconds while there is no link.

A guest who is called lets the caller in only if the owner's room lists a person at that address and port who has no link yet, and tailscaled knows the address. A caller who is ahead of the owner's word waits at the door for ten seconds. Nobody calls an address that isn't on the tailnet, whoever names it.

## The control stream

Each link has one reliable, ordered stream of messages. A message is a `u32` body length, a `u16` kind, and the body. Numbers are little-endian. A body is at most 4096 bytes: a longer length breaks the stream as soon as it is seen. Text is UTF-8 with a `u8` length, and a reader refuses text with control characters or characters that reorder or hide what is shown.

| Kind | Body | Who may send it |
|---|---|---|
| 1 Hello | version, room id (16 bytes), invite code, name, the port the sender listens on | Both ends, once, first. A caller sends it at once; the called end answers when it has let the caller in or asked its owner about them |
| 2 Welcome | your place, the room | The owner, to a guest it accepted |
| 3 Refuse | why (a number), and a sentence to show | The called end, before it hangs up |
| 4 Room | room id, turn, owner's place, 1 to 4 people: place, name, login, address, port, away, spot, the avatar they have on | The owner, to every guest, whenever the room changes. `turn` counts up; an older one is passed over |
| 5 Away | playing a game, headset off, or back | A guest to the owner, who puts it in the room |
| 6 Leave | nothing | Anyone, before hanging up. From the owner it ends the room |
| 7 Panel | id, size in pixels, place, turn, width in metres, curve, name | A guest to the owner: share my panel, or it is here now. The owner puts it in the room |
| 8 Unpanel | id | A guest to the owner: take my panel back |
| 9 Watch | panel id, level (hidden, in view, focused), whether a keyframe is needed | Anyone in the room, to the panel's owner |
| 10 Look | the avatar the sender has on: its file's SHA-256 and size, or none | A guest to the owner, who puts it in the room |
| 11 Want | a file's SHA-256 | Anyone in the room, to the person who has that avatar on: send me its file |

The Room message lists the shared panels after the people (16 at most). A panel's id is its owner's place times 8 plus its number, and a headset shares numbers 0 to 2. The owner takes a Panel or Unpanel only for the sender's own panels, and a Watch is taken only by the panel's owner. A link may say 40 of these three a second: a panel being dragged says about four.

A Hello of another version is read no further than its version, and answered with Refuse (why 1) and a sentence asking to update. Kinds a reader doesn't know are skipped when they come from someone in the room, so a later version can add messages.

A link that says anything it may not is closed: a stranger who says anything but Hello, a second Hello, a Room or Welcome from anyone but the owner, a message that doesn't read. A link gets ten seconds to say Hello, and at most eight links wait to at once.

## The avatars people have on

The room lists what each person has on: the SHA-256 and the size of their avatar's model file ([avatar/README.md](../avatar/README.md)), or none, and then the others see the abstract figure. An owner's is in its room at once. A guest tells the owner (Look), and again every two seconds until the room has it. Whoever finds an avatar in the room whose file they don't have asks its wearer for it on their own link (Want), and the wearer sends it on a one-way stream: a byte that says what the stream carries (3), then the file to the stream's end, at most 24 MB.

- **What is sent.** Only the model the wearer has on now (`wear NAME`), only if its file says its owner lets peers have it (the "may be sent" mark, set at import) and it is no larger than 24 MB, and only to someone in the room who asked for exactly that file. Any other model is worn in private. A link is sent three files at once and then one every 30 seconds, one at a time, behind the pictures and the voices.
- **What is taken.** A file is read from a link only after it was asked for there, and no further than the size the room gave. It is kept only if it came from the person the room says has it on, and its size and SHA-256 are the room's. ft-together checks nothing else and never reads into a file: the program that draws it reads it with the model file's own strict reader. A file that didn't come is asked for again after 30 seconds and the time it takes at 64 KB a second.
- **Where it is kept.** In `$XDG_RUNTIME_DIR/SOCKET-looks` (`ft_together-looks`), a folder only this user can open, as `SHA256.ftmodel`: in memory, for the session, never stored. Four files at most. A file goes when nobody in the room has it on any more, when the room is left, and when ft-together ends; what a killed one left is cleared at the next start. Without `$XDG_RUNTIME_DIR` nothing is kept.
- A link may say six Looks and Wants at once and one a second after that; more, and it is closed.
- **Who says and who draws.** `ft-avatars --room` tells ft-together what the wearer has on (`wear NAME`, from `AVATAR_MODEL` in frametop.conf, every other second), asks where the others' files are (`looks`), and draws them. It reads a file with the model file's strict readers and refuses one that isn't a whole avatar or isn't marked as one its owner lets peers have ([avatar/README.md](../avatar/README.md), "People in a shared room").

## A shared panel's video

Each picture goes on a one-way stream of its own: a byte that says what the stream carries (2), the panel's id, the picture's number (`u32`, counted per panel), flags (1: a keyframe), then the encoded picture to the stream's end, at most 1 MB.

A panel is encoded once for everyone who watches it. A viewer is in step with that video or waits for its next keyframe: they wait when they begin to watch, when they say their decoder lost its place, and when a picture sent to them was given up. The encoder is asked for a keyframe when someone waits, at most twice a second, and the keyframe puts everyone in step. A panel nobody watches is not sent, and its encoder is told so.

A viewer takes a picture only from the link of the panel's owner, for a panel that is in the room and that a program on the viewer's Frame shows, and at most 120 a second for a panel. Pictures can overtake one another, so the viewer puts them back in order: a keyframe passes when it is newer than the last picture shown, any other picture only right after the one before it, and a few early ones are held until the gap fills. When pictures are held and nothing has passed for 0.3 s, the viewer asks for a keyframe. A viewer says what it has of each panel again every second, so an owner whose link came late learns it.

The bitrate of a panel starts at 2 Mbit/s. Two pictures given up within a second bring it down to 70%, not below 300 kbit/s; five seconds of sending with none given up bring it up by 15%, to the owner's cap (3 Mbit/s until the cap can be picked).

## A shared panel on a Frame

Four programs make a shared panel, each with one job. ft-roompanels runs as a unit ("Running it"); ft-screens starts `ft-sharepanel` and `ft-peerpanel` when a screen is shared or a panel comes.

```
ft-sharepanel --panel K --size WxH [--fps F] [--socket NAME] (--fd N | --pattern)
ft-peerpanel  --panel ID [--socket NAME] (--fd N | --probe [--frames N] [--seconds S])
ft-roompanels [--socket NAME] [--together NAME] [--screens NAME]
```

**The sharer's side.** ft-screens runs one `ft-sharepanel` per shared screen (`share <N> start <slot> <W>x<H> <fps>` on `@ft_screens`; `share <N> stop`; `shares` lists them). The screen's buffers go to it as DMA-BUFs, each described once, and then `frame <id>` whenever one holds a new picture; ft-screens keeps that buffer as it is until the program says `done`, sends one picture at a time and at most the stream's rate of them, and ends a program that keeps a picture for two seconds (`together/media/screenfeed.h` has the words). `ft-sharepanel` scales the picture to the stream's size on the GPU, encodes it, and hands it to ft-together's media socket. While nobody watches the panel it asks for no pictures, and after a few seconds closes its encoder. A screen someone watches is drawn at the rate of a screen in view even when its owner looks away. Only the desktop's own screens can be shared so far: a floating window's picture is a part of its buffer, and a remote display's is another program's.

**The viewer's side.** A panel of someone else's is a remote screen of ft-screens' (`remote <N> peer <panel> <W>x<H> <metres> [label]`), whose program is `ft-peerpanel` in place of ft-stream. It takes the panel's pictures from ft-together, decodes them with the hardware decoder into a ring of three RGBA buffers as a remote display's are, and tells ft-together how much of the panel its viewer sees and when its decoder needs a keyframe. Pointer and key input on such a panel goes nowhere yet (M4).

**ft-roompanels** keeps both in step with the room, four times a second while this Frame is in one:

- The floor. Where this Frame's own floor lies in the room is ft-together's to remember (`floor`). If nobody has said, ft-roompanels lays it from where the head is (ft-screens' `head`) and your spot, with the same sums `ft-avatars --room` uses, so whichever asks first, the answer is the same. With no head pose for five seconds (the headset off) it lays the floor's middle on the spot.
- The others' panels. Each gets a remote screen, counted down from the last one (116) so ft-layout's remote displays, which count up from 101, keep their numbers. It is put where the room has it when it is made and whenever the room moves it; between those it stays where its viewer leaves it. A panel that left the room, or whose video changed size, goes. A peer's name for a panel is cut down to plain characters before it goes into a command.
- Your panels. `share N` on `@ft_roompanels` puts screen N of this desktop into the room (three at most), `unshare N` takes it back, `state` says what is shared and shown as JSON. The video is the screen made to fit 1920x1200, 30 pictures a second. Where the screen stands here is read from ft-screens and told to the room whenever it changes, so moving your screen moves it for everyone.
- Leaving the room, or ft-roompanels ending, takes the room's panels away and stops every share. One that starts finds and clears what an earlier one left.

What isn't there yet: a viewer can move their own copy of someone's panel until the room next moves it (who may move what is M4); the video's size is fixed when a screen is shared; with more than about eight remote displays the numbers would meet; Display Settings lists a peer's panel as a remote screen.

## Voices on a Frame

```
ft-roomvoice [--socket NAME] [--together NAME] [--screens NAME] [--muted] [--hum HZ] [--probe]
```

One program sends the wearer's voice to the room and plays the others'. It runs as a unit next to ft-together ("Running it").

- **Your voice.** The session's default microphone, which on the Frame runs through echo cancelling (it shows in the mixer as "Your voice in the room"), 20 ms at a time into Opus: mono, 24 kbit/s, with what the decoder needs to make up a lost packet from the next one. Silence isn't sent, but for a small packet every 400 ms that tells how the quiet sounds. The microphone is open only while you are in a room, not muted, and the headset is on your head: away in a game your voice stays on, with the headset off (`away off`) it doesn't.
- **Their voices.** Each person's packets wait about 60 ms in a buffer of their own (`voice/jitter.h`), so ones that come late or in a bunch still play in step. A lost one is made up from the next when that is there and guessed otherwise; after 100 ms of guessing with nothing waiting the voice stops until its next packet, and a voice more than 240 ms behind jumps to now. Packets are decoded here and nowhere else, and only ones that hold 20 ms.
- **Where they come from.** Each voice is heard from its owner's head (their pose's eyes, the same poses `ft-avatars --room` draws), by where your own head is on your floor (ft-screens' `head`, ten times a second) and where your floor lies in the room (`floor`). Further away is quieter, to a fifth at most, to the side is louder in that ear, and behind is a little quieter. It is loudness only, with no delay between the ears and no shaping by the head. Someone whose place isn't known, or anyone while your own head isn't, is heard in the middle. The mix shows in the mixer as "The room's voices".
- **Questions** on `@ft_roomvoice`: `mute`, `unmute`, `quit`, and `state`, which answers `ok` and JSON: `room`, `muted`, `mic` (`open`, `closed`, `failed`), `sent` and `level` (your voice), `playing`, and for each other person `person`, `speaking`, `placed`, `level`, `played`, `lost`, `late`.
- `--hum HZ` and `--probe` are for tests: the wearer hums a note in place of the microphone, and what would be played is measured (in `state`, as `out`) in place of the loudspeakers.

What isn't there yet: a volume for each person and for the room, the microphone's level made even, mute kept across runs or shown in the room, the avatars' mouths moved by these voices (the pose's mouth shapes are still sent shut), and how loud someone is shown on their figure. A steady tone is taken for noise by the encoder and not sent, which is right for a fan and wrong for a held note.

## Away

```
ft-roomaway [--together NAME] [--helper NAME] [--relay NAME]
```

While ft-together runs, `ft-roomaway` asks the pointer helper whether the headset is worn (`gaze ? headset`) and the input relay whether Frametop is paused for a game (`pause ?`), once a second in a room and every other second outside one, and tells ft-together `away off`, `away game` or `away back` whenever that has it otherwise. It tells outside a room too, so a room that is opened or joined with the headset off has its wearer away from the start, and no microphone opens for a headset that lies on a desk: a program here reads how its own wearer is away from `state`'s `away`, not from the room's copy of them, which trails it by a trip to the room's owner and back. The headset off wins over a game. An answer stands for five seconds; with none, the wearer is taken to be there, so a Frame without the pointer helper running isn't away for good. What follows from being away is each program's own: `ft-roomvoice` closes the microphone and plays nothing with the headset off, and a paused Frametop draws no screens, so a shared one stands still.

## The voice datagram

One datagram per person for each 20 ms they are heard, at most 403 bytes: a `u8` kind (2), a `u16` count, one more for each 20 ms the sender's microphone gave whether sent or not, and one Opus packet of 1 to 400 bytes. The count puts the packets in order and tells how long a gap is. ft-together passes it between a link and this Frame's programs without reading the packet; whoever plays it hands it to the Opus decoder, which takes any bytes.

## The pose datagram

One datagram per person, about 30 times a second, at most 215 bytes: a `u8` kind (1), a `u16` count that tells a newer one from an older, the eyes' place, the head's turn, the eye height when standing, a flag nibble per hand, five mouth shapes, then for each tracked hand the wrist, its turn, fifteen finger bone turns when its fingers are tracked, and the elbow if the sender knows it. Places are in the room's coordinates. These are the inputs of the body solver ([avatar/README.md](../avatar/README.md)), not its result: every Frame runs the same solver on them.

A hand's turns belong to no body. The hand's turn is the frame of its palm, and each finger turn is the frame of that bone, all in the room's directions: which way the bone points and which way its back faces. The sender has them from its hand tracker, and each receiver fits them to the body it draws for that person, whose bones may rest differently from anyone else's. So the same datagram poses the abstract avatar and, once avatars are sent, the person's own.

A reader takes a datagram only if every place is within 100 m of the room's middle, every turn is close to unit length, the eye height is between 0.5 and 2.5 m, the flags make sense together and no byte is left over.

## Tests

`just test-c` builds these with the address and undefined-behaviour sanitizers and runs them. The links' test, the loopback test and ft-roompanels', ft-roomvoice's and ft-roomaway's tests between two `ft-together` need msquic, which `together/build.sh` builds; without it they are SKIPPED. `tests/roomkit.py` is what the Python tests share: one `ft-together` on the loopback and the questions to it, a program on its media socket, a program whose lines are read as they come, a pose and a voice datagram, and a program that watches and listens.

| Test | What it covers |
|---|---|
| `tests/proto-test.cpp` | Every message and the pose datagram there and back, each cut short at every length, damaged, and made of noise; the stream parser fed in pieces of any size; panels alone and in a room |
| `tests/room-test.cpp` | Invites (once, ten minutes, eight waiting, the wrong-guess limit), spots, whose panels a room takes, and the allowance |
| `tests/session-test.cpp` | Up to five sessions run against each other on a network the test makes (`tests/fakenet.h`): joining, declining, a full room, the guests' links, leaving, a lost owner, another version, panels shared, moved and taken back, who is told who watches, the avatar each has on (before a room, changed, a lost word said again, a file asked for), and guests and strangers who say what they may not. Three hundred rounds of noise and forged messages on every kind of link |
| `tests/flow-test.cpp` | The video's small rules: who gets the next picture, the order a viewer decodes them in, the bitrate, and a picture's bytes on its stream |
| `tests/video-test.cpp` | Three sessions on the test's network, each with the video's rules, a wire the test can lose, hold back and refuse pictures on, and programs that remember what they were told: who is sent what and when, keyframes asked for, pictures out of order, the bitrate, a guest's panel over the guests' own link, and pictures nobody may send |
| `tests/looks-test.cpp` | Three sessions on the test's network, each with the avatars' rules, files the test carries, and a closet that remembers: a file asked for once and from its wearer only, kept when its hash and size are the room's, taken off, changed, asked for again when it didn't come, gone on leaving; and files nobody asked for, from the wrong person, of the wrong size or content, asked for too often, or asked of someone who doesn't have them on |
| `tests/wardrobe-test.cpp` | The avatars' files on a Frame, in folders the test makes: which model the room may hear of (marked, unmarked, missing, not a model, too large, a name that isn't one), a file read again only when it changed, and the files that came: kept whole in a private folder, four at most, gone when unworn and when the program ends, an earlier run's leftovers cleared, and nothing kept in a folder that is a link or without a session folder |
| `tests/tailnet-test.cpp` | tailscaled's answers, kept in the test: this node's address and name, who is at an address, Tailscale stopped or logged out, answers that aren't what they should be; the JSON reader on good texts, nearly good ones and noise; which addresses are on the tailnet; a name looked up |
| `tests/local-test.cpp` | The pose gate (what passes, an older pose, a flood, a link forgotten), ft-together's own count, the voice datagram and its gate (the sizes it takes, an older one, the count going round, a flood), the watchers and the listeners (four at most, forgotten after five seconds, what each is sent), the floor (the first said stands, lifted on leaving), and the media socket's messages however the socket cuts them up |
| `tests/links-test.cpp` | Real links between ends in one program, on loopback: bytes in order both ways, datagrams, closing, calls that may not be made or that nobody answers, a call to another address of this machine, the limits on what a far end can make an end hold, and taking an end down with links up. Pictures: whole either way, the longest, a burst, untaken ones bounded, and on a network that stops carrying, each given up after 150 ms and told, never arriving late. Files: one that wasn't asked for isn't taken, one that was arrives whole and once, a longer one than asked for is refused, the largest both ways beside pictures and datagrams, and a link closed or an end taken down with files on their way |
| `tests/loopback-test.py` | Four `ft-together --loopback` programs: a room opened, a wrong code, a declined ask, noise on the port, two guests let in and linked to each other, away, a guest whose program is killed, leaving and coming back, a second program on the same socket, and the owner's program ending. Poses: one handed to a program reaches the watchers of the others' programs with whose it is and that program's own count, from the owner and from a guest, and nothing reaches a program nobody watches, a pose's own sender, or anyone after leaving; each program keeps its own floor. Voices: one handed to a program reaches the others' listeners as it was sent, with whose it is, through the count going round; a late one, one that isn't a voice, and a person's own never do. Avatars: a model the owner or a guest has on comes to the others as a file, whole (up to two megabytes here) and where only they can read it, a newcomer gets it too, an unmarked one stays private, and the files go when it is taken off, on leaving, and when the program ends. Panels: shared by the owner and by a guest, in everyone's room, gone with their owner. Video: test programs on the media sockets stand in for an encoder and two viewers' programs, and pictures cross real links in order and byte for byte, up to a megabyte; a second viewer waits for its keyframe; nobody watching is told to the encoder; and programs that say what they may not, say nothing, or don't take their pictures are let go |
| `tests/test_roompanels.py` | ft-roompanels' rules without either program (`just test-python`): the floor's sums there and back, a guest across the room seeing a panel's back; the keeper against stand-ins that answer as ft-together and ft-screens do: the floor laid from the head or without one, a peer's panel made, placed once, moved with the room, gone with it, made again at another video size, remote displays left alone, hostile names, what an earlier run left; a screen shared, moved, taken back, three at most; leaving and ending; ft-together silent for a moment |
| `tests/roompanels-test.py` | `ft-roompanels` itself between two real `ft-together --loopback`, each with a stand-in ft-screens on a socket (`tests/fakescreens.py`): each floor laid, a screen shared by the owner and one by the guest standing at the other's where the room says (each side's pose taken back to the room through its own floor), moved, taken back, gone when its owner leaves, and nothing left behind when ft-roompanels ends |
| `tests/voice-test.cpp` | The voices' parts without a sound device: the buffer (in step, late, in a bunch, lost, a pause, a stall, the count going round, a voice far behind), where a voice is heard from, Opus there and back (the note that went in comes out, silence isn't sent, a lost packet made up, four thousand packets of noise, a packet that isn't 20 ms), and the mix (two voices, each in its ear, never past full scale, a person forgotten) |
| `tests/roomvoice-test.py` | `ft-roomvoice` itself, twice, between two real `ft-together --loopback`, each with a stand-in ft-screens, humming a note of its own and measuring what it would play: nothing outside a room, one's note heard by the other and not by themselves, from the side its owner stands on and quieter from further off, in the middle when their place isn't known, mute and unmute, the headset off and a game, leaving, and ending cleanly |
| `tests/test_roomaway.py` | ft-roomaway's rules (`just test-python`): the helper's and the relay's answers read, good and not; the headset off, a game, both, back; what the room has put right; an answer that stands five seconds; told outside a room too, and again to an `ft-together` that started again |
| `tests/roomaway-test.py` | `ft-roomaway` itself next to a real `ft-together` in a room with another, with stand-ins for the pointer helper and the relay: off, a game and back reach the other end's room, what another program said is put right, a helper that stops answering, leaving, and ending cleanly |
| `tests/test_roompage.py` | The Settings page (`just test-python`): the answers read, good and not; what is typed to join; and the backend and the page itself driven against stand-ins for the four programs on sockets of the test's own, with a made-up systemctl: the switch, a room opened, an invite, an ask let in, joining, mute, a screen shared, and no QML warnings |
| `tests/test_units.py` | The units as files: the room's four go with ft-together's and are never enabled themselves, each starts a program there is, each cleans up only its own, and `run.sh` and `install.sh` name them all |
| `screens/tests/share-test.c` | ft-screens' share tap with made-up buffers, the test being the `ft-sharepanel` it starts: the commands, each buffer described once, one picture out at a time and the newest next, the rate, a program that keeps a picture or dies, pictures only while watched |
| `screens/tests/remote-test.c` | With the remote displays' cases: a room's panel is given its id and the room's socket, waits for an old program of its own screen and for no host's queue, and what isn't one is refused |
| `tests/panels-test.py` | **Run by hand, not by `just test`, and not at all for now** (docs/together.md, "Open questions": the Frame reset during its first run). The real video: `ft-sharepanel --pattern` to `ft-peerpanel --probe` through two `ft-together`, checking every picture's size, number and colours, the encoder closing when nobody watches, a viewer coming back, the sharer starting again, and several sizes. It needs the Frame's encoder and decoder |
