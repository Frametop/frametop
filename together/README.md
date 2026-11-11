# Frametop Together

Shared rooms: up to four people in one owner's room, each on their own Frame, over Tailscale. The design, its decisions and the milestones are in [docs/together.md](../docs/together.md). This file says what is in this folder and what goes over a link.

Built so far: the spikes (M0), and from M3 the session's wire format and rules, the links, `ft-together`, which opens a room, lets people in and keeps the room's people linked, and poses: each person's head and hands go to the others, and `ft-avatars --room` ([avatar/README.md](../avatar/README.md)) draws them, in the avatar they sent or as abstract figures. Shared panels: the room's list of them, the video's path through `ft-together` from the program that encodes a panel to the programs that show it, and its rules are done and tested. The programs at each end (`ft-sharepanel`, `ft-peerpanel`, ft-screens' share tap and peer screens, `ft-roompanels`) are written and tested without video; the video through the hardware encoder and decoder has not run yet ("A shared panel on a Frame" below says why). Voice is not built yet.

## What is where

| Folder | What it holds | Depends on |
|---|---|---|
| `proto/` | The bytes on a link: bounded readers and writers (`bytes.h`), the control stream's messages (`messages.h`, with the avatar a person has on as the room lists it), a shared panel as the room lists it (`panel.h`), a picture on its stream (`framewire.h`), the pose datagram (`posewire.h`), the voice datagram (`voicewire.h`), the pointer datagram (`pointwire.h`), and control of a shared panel asked for, given and used (`control.h`). No I/O, standard library only, so the PC sender in the Vibepollo fork can take the folder as it is | nothing |
| `room/` | A room's own rules: invites (`invites.h`), where people stand (`seats.h`), whose panels are in it (`panels.h`), how often someone may do a thing (`allowance.h`), who was given control of which panel and for how long that stands (`grants.h`), and whose turn it is on the one desktop a Frame has (`desk.h`) | `proto/` |
| `session/` | The session: who may join, who is in the room, who links to whom, and what each link may say (`session.h`; `session.cpp` has what the links bring, `wearer.cpp` what this Frame's wearer does). The network, the tailnet, the clock and the dice are handed in. Messages it doesn't read itself it hands on from people in the room (`Config::said`) and sends for whoever knows them (`Tell`), which is how control's travel. `commands.h` puts a session behind lines of text | `proto/`, `room/` |
| `flow/` | The shared panels' video, its rules: who gets a panel's next picture (`sendgate.h`), the order a viewer decodes them in (`framegate.h`), the bitrate (`pace.h`), and the path through ft-together between the links and this Frame's media programs (`video.h`). And the avatars people have on, their rules (`looks.h`): who is asked for a file, who is sent one, and whose is kept. And control of the shared panels (`control.h`): who asks, who was given what, whose input reaches this Frame's desktop, and what is let go for whom. The links, the programs, the files and the desktop are handed in | `proto/`, `room/`, `session/`, OpenSSL (SHA-256) |
| `local/` | What ft-together does for this Frame's own programs beyond the session's commands: the poses and voices it lets through from a link (`posegate.h`, `voicegate.h`, both on the count and the allowance of `linkgate.h`), the wearer's mouth it puts into their poses (`mouth.h`), who gets the others' poses and voices (`watchers.h`), the others' pointers on their way in and the wearer's on its way out (`pointgate.h`), the others' input on its way to the program that gives it to the desktop (`inputs.h`), control behind lines of text (`controlask.h`), where this Frame's floor lies in the room (`floor.h`), where the others were last seen in it (`whereabouts.h`), the datagrams on the local socket that aren't text (`datagrams.h`), the media programs on their own socket (`media.h`, with what is said there in `mediawire.h`), and the avatars' files on this Frame: the wearer's own and the ones that came (`wardrobe.h`). And one thing a program does for ft-together: the wearer's hand on someone else's panel, from ft-screens' words to a pointer and `control act` lines (`panelhand.h`, sent by `handsend.h`) | `proto/`, `flow/`, `avatar/modelfile.h`, `conf/ftsock.h` |
| `net/` | The real network: the links on msquic (`quiclinks.h`; `quiclinks.cpp` has the connections, the control stream and the datagrams, `quicframes.cpp` the streams that carry a picture each, `quicfiles.cpp` the ones that carry an avatar's file), with the self-signed certificate QUIC wants (`selfcert.h`), which address a name stands for (`lookup.h`), and who is at an address by tailscaled's word (`tailnet.h`, with the small JSON reader for its answers, `json.h`) | `session/`, msquic, OpenSSL |
| `ft-together.cpp` | The program: one loop over the links and the local sockets | `session/`, `flow/`, `net/`, `local/` |
| `ft-room` | Asks a running ft-together one question from a shell: `together/ft-room state`, `together/ft-room open`, `together/ft-room --socket NAME invite` | `conf/ftipc.py` |
| `media/` | The iris hardware encoder (`irisenc.h`), and what `ft-sharepanel` is made of: the encoder's input buffers (`pictures.h`: as long as the encoder reads, and out of the CPU's caches), the GPU pass that draws a screen's picture into one (`blit.h`), the encoder for one panel (`panelenc.h`), and where pictures come from (`feed.h`): a screen of ft-screens' (`screenfeed.h`) or a test picture (`patternfeed.h`, `testpattern.h`). `words.h` reads the lines the programs trade with ft-screens | V4L2, GBM, EGL |
| `ft-sharepanel.cpp` | Encodes one screen this Frame shares | `media/`, `local/mediaclient.h` |
| `ft-peerpanel.cpp` | Decodes one panel someone else shares, into a remote screen of ft-screens', and passes the wearer's hand on it to ft-together | `stream/iris.h` (the remote displays' decoder), `local/mediaclient.h`, `local/handsend.h` |
| `roompanels/` | ft-roompanels' rules: the sums between this Frame's floor and the room (`placing.py`), what it keeps true (`keeper.py`) and moving the wearer through the room (`moving.py`) | Python's standard library |
| `ft-roompanels` | The program around them: its loop and its socket | `roompanels/`, `conf/ftipc.py` |
| `roomaway/` | ft-roomaway's rules: what the pointer helper and the relay say of the wearer, and what the room is told (`teller.py`) | Python's standard library |
| `ft-roomaway` | The program around them: tells the room when the headset comes off or a game starts | `roomaway/`, `conf/ftipc.py` |
| `roominput/` | ft-roominput's rules: an act ft-together passed on, read into a `peer` command for ft-screens on the screen its panel is (`giver.py`) | Python's standard library |
| `ft-roominput` | The program around them: takes the others' input from ft-together and gives it to the desktop | `roominput/`, `conf/ftipc.py`; ft-screens' side is `screens/peerinput.h` |
| `points/` | What `ft-roompoints` is made of: the last word of each person's pointer and how long it stands (`marks.h`), which of ft-screens' panels shows each of the room's (`panelkeys.h`), where a point of a picture is on a flat or bent panel (`surface.h`), the mark's picture (`dotimage.h`), the socket that asks for both (`asker.h`), and what draws: SteamVR overlays (`dots.h`) or, for tests, nothing (`drawer.h`) | `proto/pointwire.h`, `local/datagrams.h`, `avatar/abstract/colour.h` (each place's colour), OpenVR |
| `ft-roompoints.cpp` | Draws where the others point, on the panels they point at | `points/` |
| `roompage.py` | Frametop Settings' Together page without Qt: the programs' answers read into what the page says, and what is typed read into a `join` | Python's standard library |
| `roomavatar.py` | The avatar the wearer has on, for the page, without Qt: which one frametop.conf names, what its file says of it, and the command that marks it as one the others may have | `conf/ft_conf.py`, `avatar/ftmodel.py` |
| `ft_together_settings.py`, `TogetherPages.qml` | The page: its backend, which asks the room's programs off the UI's thread, and its QML | `roompage.py`, `roomavatar.py`, `conf/ftipc.py`, `conf/fthost.py`, PySide6 |
| `frametop-together.service`, `frametop-room-*.service`, `run.sh` | The user units the programs run as, and their installer | `scripts/install-lib.sh` |
| `voice/` | What `ft-roomvoice` is made of: Opus each way (`codec.h`), the buffer that plays a voice's packets in step however they arrive (`jitter.h`), where a voice is heard from (`spatial.h`), everyone's voices mixed for two ears (`voices.h`), the thread that sends the wearer's (`sender.h`), with what it makes of their mouth (`mouthsay.h`, on `avatar/face/face.h`), and the one that plays the others' (`player.h`), the sound devices behind two small interfaces (`sound.h`, on PulseAudio in `pulse.cpp`, pretended for tests in `pretend.h`), and where the wearer's head is (`headask.h`) | libopus, libpulse-simple |
| `ft-roomvoice.cpp` | The room's voices on this Frame | `voice/`, `avatar/room/client.h` |
| `spike/` | The M0 spikes, `ft-enctest` and `ft-quictest` | `media/`, `net/` |
| `tests/` | The tests below | |

`together/build.sh` builds `build/ft-together`, `build/ft-sharepanel`, `build/ft-peerpanel`, `build/ft-roomvoice`, `build/ft-roompoints` and msquic (pinned by commit) in the dev container, and with `--spikes` the spikes too.

## Running it

Frametop Together is a part of its own: `install.sh` offers it (no by default while it is experimental; `--together` and `--no-together` answer for it), and `together/run.sh install` installs it later. Installed, it is off. The switch on Frametop Settings' Frametop Together page turns it on, now and at every login, as `together/run.sh on` does.

| Unit | Runs | Where |
| --- | --- | --- |
| `frametop-together.service` | `ft-together` | the container |
| `frametop-room-people.service` | `ft-avatars --room`: the others, drawn, and your own pose told | the container; it waits for SteamVR |
| `frametop-room-panels.service` | `ft-roompanels` | the host |
| `frametop-room-voice.service` | `ft-roomvoice` | the container |
| `frametop-room-away.service` | `ft-roomaway` | the host |
| `frametop-room-input.service` | `ft-roominput` | the host |
| `frametop-room-points.service` | `ft-roompoints` | the container; it joins SteamVR only while someone points at a panel shown here |

Only the first is ever enabled. It wants the other six, and each of them is part of it, so they start, stop and restart with it and never run without it. Outside a room they wait: none of them opens the microphone, draws, or encodes anything until ft-together says this Frame is in one. `together/run.sh status` shows the seven, `log` their journal, and `off` stops them and leaves a room this Frame is in (its own room ends for everyone). ft-together takes calls only on the tailnet address, and answers nobody who isn't on the tailnet (below, "The links").

`scripts/fast-install` builds `together/` as a component, and a release brings it built (`pack/Containerfile`); msquic is fetched at the commit `together/build.sh` pins.

### The page

Frametop Settings > Frametop Together (`ft_together_settings.py`, `TogetherPages.qml`) is how a room is used from inside the headset:

- The switch, with what ft-together says when it can't run ("Tailscale isn't running on this headset"), and this headset's tailnet address to give to a guest.
- In no room: Open a room, or join someone's with their headset's address (`name`, `name:port` or `name port`) and the invite code they gave you.
- In your own room: Make an invite code (shown until the room ends, with a copy button), and each person who asks to come in with Let in and No. Closing the room asks first when others are in it.
- In any room: who is in it and how (you, the owner, in a game, headset off, connecting), and Leave.
- What you share: your voice (the switch mutes `ft-roomvoice`), your avatar, and a switch per screen of the desktop (`share N` and `unshare N` to `ft-roompanels`), three at most.
- Your avatar: the row says how the others see you, and why when it is the abstract figure (no avatar on, hands only, a VRM whose licence says no, a file too large). A model that can't say whether it may be passed on (a glTF or an FBX) stays on this headset until you say so: "Let them have it…" asks once, and a yes marks the model's file (`ft-model-import --allow-share`, [avatar/README.md](../avatar/README.md), "Licences"). ft-together reads the file again the next time `ft-avatars --room` says what you have on, and the room has it within a few seconds.

The page asks `state`, `here` and `looks` of ft-together, `state` of ft-roomvoice and ft-roompanels and `screens` of ft-screens once a second while it shows, on a worker thread, and nothing while it doesn't. Names, logins and notes in the answers are text a peer chose: they are shown as plain text, and what is typed for a `join` is sent only when it reads as an address and a code (`roompage.py`).

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
| `floor move X Z YAW` | Puts the floor that stands somewhere else, which is how the wearer moves through the room ("Moving yourself", below). The answer is the new one, and it also goes at once, as the same line of text, to every program that watches or listens, so none of them draws or hears the room from the old floor until it next asks. Refused while no floor is laid |
| `where` | Where the others were last seen: `ok COUNT PLACE:X:Y:Z:YAW ...`, a person's place in the room, where their eyes are in the room (metres) and which way they face (radians, left of the room's forward), from the last pose of theirs that passed the gate. Someone who sent none for two seconds isn't listed |
| `control`, `control ask`, `control give`, `control take`, `control act`, `control own`, `control here` | Control of the shared panels: "Control of a shared panel", below |
| `inputs` | `ok`, and for the next five seconds the input the others may give this Frame's desktop comes to the asker (below). The program that gives it to the desktop asks again each second. One program at a time: a second is told no |
| `points` | `ok`, and for the next five seconds every pointer datagram a person in the room sends comes to the asker (below). Asked again each second, like `watch`. At most four at once |

Eight kinds of datagram on the socket aren't text, and are told by their first byte, which no text starts with:

- **Your own pose**, to ft-together: a pose datagram as it goes on a link (first byte 1, below), in the room's directions. ft-together reads it as it reads a peer's, puts its own count and your mouth in it and sends it to everyone in the room it has a link to. It isn't answered, and outside a room it goes nowhere.
- **Your own mouth**, to ft-together: the byte `0x83`, then the weights of its five shapes now (aa, ih, ou, ee, oh, 255 whole), six bytes in all. It comes from the program that hears you, and ft-together puts it into each pose of yours it sends from then on (`local/mouth.h`). A mouth that isn't told again for half a second shuts, so one left open by a program that ended doesn't stay open. It isn't answered.
- **Someone's pose**, to each watcher: the byte `0x81`, their place in the room (a byte), then their pose datagram. Only a pose from a link whose person is in the room is passed on, only if it reads, is newer than that link's last, and is within 90 datagrams a second from that link (three times what a Frame sends), rubbish counted.
- **Your own voice**, to ft-together: a voice datagram (first byte 2, below). It is sent as it is to everyone in the room ft-together has a link to. It isn't answered, and outside a room it goes nowhere.
- **Someone's voice**, to each listener: the byte `0x82`, their place in the room, then their voice datagram. Only one from a link whose person is in the room is passed on, only if it reads, is newer than that link's last, and is within 150 a second from that link (three times what a Frame sends). ft-together doesn't look into the Opus packet.
- **Your own pointer**, to ft-together: a pointer datagram (first byte 3, below). ft-together reads it, puts its own count in it and sends it to everyone in the room it has a link to. It isn't answered, and outside a room it goes nowhere.
- **Someone's pointer**, to each program that asked `points`: the byte `0x84`, their place in the room, then their pointer datagram. Only one from a link whose person is in the room is passed on, only if it reads, is newer than that link's last, and is within 90 a second from that link.
- **Someone's input**, to the program that asked `inputs`: the byte `0x85`, their place in the room, then ten bytes: the panel's id, what it is (1 a move, 2 a button, 3 a scroll, 4 a key), and two `i32` ("Control of a shared panel" has what they mean). Only input ft-together took is passed on: for a panel of yours with a grant that stands, from the person it was given to, while it is their turn. Nobody is waited for, so a program whose queue is full misses some, and a missed release would leave a button down. The first input after one was missed is therefore preceded by one that says let go of everything (what 0, from place 255).

**Your floor in the room.** Everyone tracks themselves on their own floor, and the room has its owner's directions. A place on your floor, turned by YAW radians about the way up and then moved by X and Z metres, is that place in the room. It is laid once, when you come in, so that where you stand then is the spot the room gave you, facing the way the spot faces, and after that it changes only when you move yourself through the room (`floor move`). Every program of yours that draws the room or tells it where you are needs the same answer, so ft-together keeps it: the first program that knows where you stood says it, and the rest ask.

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

`--loopback` is for tests: no Tailscale, the links are on 127.0.0.1 only, and everyone there is let in as `someone@loopback`. A fake peer for the live Frame is a second `ft-together` in the normal mode on another port and socket: it has this Frame's tailnet address, and the session tells people apart by address and port. The testbench runs up to three of them, each with a pretended wearer and a hummed voice, and lets them into this Frame's room ([testbench/docs/peers.md](../testbench/docs/peers.md)).

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

A headset is in a room once. The owner hears that a link was lost up to ten seconds after the guest does, and a guest can ask again within that time. A headset the room still lists, by its address and port, takes no second place: when it is let in again, the owner closes its old link, takes its old place out of the room, and gives it a place as it does anyone. It also asks for the place it has, so it is not told that a room it fills is full. The room's rules are checked by a model: [docs/charts](../docs/charts/README.md), group G9.

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
| 12 AskControl | panel id | Anyone in the room, to the panel's owner: may I control it |
| 13 Grant | panel id, seconds | A panel's owner, to one person: it is yours for that many seconds more (30 at most), said again every three seconds while it stands. 0 seconds: it is yours no longer, or the answer is no |
| 14 Act | panel id, what (1 a move, 2 a button, 3 a scroll, 4 a key), two `i32` | Anyone in the room, to the panel's owner: my input on your panel |

The Room message lists the shared panels after the people (16 at most). A panel's id is its owner's place times 8 plus its number, and a headset shares numbers 0 to 2. The owner takes a Panel or Unpanel only for the sender's own panels, and a Watch is taken only by the panel's owner. A link may say 40 of these three a second: a panel being dragged says about four.

AskControl and Act are taken only by the panel's owner, and a Grant only from them. A link may say two AskControl or Grant a second (eight at once) and 240 Act a second (480 at once): a hand moving a pointer says about sixty. An Act for a panel with no grant, from someone it wasn't given to, or while it is someone else's turn, is dropped and the link stays: a grant can end while input is on its way. "Control of a shared panel" has the rules.

A Hello of another version is read no further than its version, and answered with Refuse (why 1) and a sentence asking to update. Kinds a reader doesn't know are skipped when they come from someone in the room, so a later version can add messages.

A link that says anything it may not is closed: a stranger who says anything but Hello, a second Hello, a Room or Welcome from anyone but the owner, a message that doesn't read. A link gets ten seconds to say Hello, and at most eight links wait to at once.

## The avatars people have on

The room lists what each person has on: the SHA-256 and the size of their avatar's model file ([avatar/README.md](../avatar/README.md)), or none, and then the others see the abstract figure. An owner's is in its room at once. A guest tells the owner (Look), and again every two seconds until the room has it. Whoever finds an avatar in the room whose file they don't have asks its wearer for it on their own link (Want), and the wearer sends it on a one-way stream: a byte that says what the stream carries (3), then the file to the stream's end, at most 24 MB.

- **What is sent.** Only the model the wearer has on now (`wear NAME`), only if its file says its owner lets peers have it (the "may be sent" mark, set at import or on the Together page once its owner said yes) and it is no larger than 24 MB, and only to someone in the room who asked for exactly that file. Any other model is worn in private. A link is sent three files at once and then one every 30 seconds, one at a time, behind the pictures and the voices.
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

**What the Frame's encoder needs.** Three rules, each found by a run that went wrong on the Frame (2026-11-11), and each kept where it can't be forgotten:

- *A width that is a multiple of 128.* A row of the encoder's picture is as long as the next multiple of 128 pixels. Given DMA-BUF pictures 1366 pixels across, the Frame reset itself in the second the encoder started, twice (2026-11-11: in `tests/panels-test.py` as that size began, and with the encoder alone, no decoder running), with nothing in the kernel's log. `irisenc::Encoder::Open` refuses any other width before it opens the device (`irisenc::Takes`), and ft-roompanels only asks for widths it takes (`roompanels/keeper.py`, `stream_size`). 640, 896, 1280, 1408, 1664 and 1920 across are known to work.
- *Buffers as long as the encoder reads.* It reads whole rows past the picture's last, to a step of 32 rows (1080 rows as 1088, 936 as 960). `PanelEncoder::Open` asks the encoder for its layout first and makes the GBM buffers that long.
- *New buffers out of the CPU's caches.* The kernel zeroes a new buffer's pages through a cached view of its own, and those lines of zeros stay in the caches after a picture has been written, uncached, to the memory under them. The encoder reads through the caches: most of the first picture drawn into each buffer was encoded black, with the picture showing only where a line had happened to fall out. `Pictures::Open` puts each buffer out of the caches once (`dc civac` over its mapping), before anything is drawn. Asking the kernel (`DMA_BUF_IOCTL_SYNC`) or writing the picture's own lines back (`dc cvac`) does not do it.

`tests/panels-test.py` is the test of all three, on the hardware; `tests/encsize-test.cpp` and `tests/test_roompanels.py` hold the first without it.

**ft-roompanels** keeps both in step with the room, four times a second while this Frame is in one:

- The floor. Where this Frame's own floor lies in the room is ft-together's to remember (`floor`). If nobody has said, ft-roompanels lays it from where the head is (ft-screens' `head`) and your spot, with the same sums `ft-avatars --room` uses, so whichever asks first, the answer is the same. With no head pose for five seconds (the headset off) it lays the floor's middle on the spot.
- The others' panels. Each gets a remote screen, counted down from the last one (116) so ft-layout's remote displays, which count up from 101, keep their numbers. It is put where the room has it when it is made and whenever the room moves it; between those it stays where its viewer leaves it. A panel that left the room, or whose video changed size, goes. A peer's name for a panel is cut down to plain characters before it goes into a command.
- Your panels. `share N` on `@ft_roompanels` puts screen N of this desktop into the room (three at most), `unshare N` takes it back, `state` says what is shared and shown as JSON, and `panels` says which screen here shows each of the room's panels (`ok COUNT ID:SCREEN:WxH ...`: your own shared screens as large as the desktop has them, the others' panels on their remote screens), which is what `ft-roompoints` draws by. The video is the screen made to fit 1920x1200, 30 pictures a second, at a width that is a multiple of 128 and a height that is a multiple of 8, as near the screen's shape as those allow (a 1366x768 screen goes as 1408x792, a 3440x1440 one as 1920x800, a 1080x1920 one as 640x1136): the encoder takes no other widths (below). Where the screen stands here is read from ft-screens and told to the room whenever it changes, so moving your screen moves it for everyone.
- Leaving the room, or ft-roompanels ending, takes the room's panels away, stops every share and puts this desktop's screens back where they stood before the wearer moved through the room (`shift back`, below). One that starts finds and clears what an earlier one left.

**Moving yourself.** You walk in your own room, and that needs nothing: your poses are told in the room's directions through your floor. To go further than your room reaches, your floor moves in the room instead, and everything that stands in the room on this Frame moves the other way on your floor, so the room stands still and you are somewhere else in it. `moving.py` does both halves, in this order: it tells ft-screens `shift DX DZ DEGREES`, which moves every screen that isn't pinned to the head or a hand or being dragged (the desktop's own, floating windows and the others' panels alike) and remembers the sum, and then tells ft-together `floor move`. If the floor won't move, the shift is undone. The others see you move because your poses come through the new floor; your shared screens stay where they are in the room, so nothing is told about them. Four commands on `@ft_roompanels` do it, each answered with the new floor (`ok X Z DEGREES`):

| Command | What it does |
|---|---|
| `go X Z DEGREES` | Puts your head at that place in the room, facing that way (degrees left of the room's forward). Your height stays your own |
| `go person N` | To 1.2 m in front of the person at place N, facing them. Where they are is ft-together's `where`; someone who has sent no pose yet is taken to be at their spot |
| `go panel ID` | To in front of that panel, level with its front, at the distance from which it fills 60 degrees of the view (between 0.6 and 3 m). A panel that lies flat is read from its lower edge |
| `pull DX DZ DEGREES` | Moves you by that much on your own floor: what grabbing the air and pulling does. At most 5 m and half a turn at once |

`go` needs to know where the head is (ft-screens' `head`), so it is refused with the headset off. A place further than a kilometre from the room's middle is refused. What isn't there yet: nothing in the headset says these commands (grabbing the air with both grips, a key and the mouse, and a list to jump from come next), `go` is a jump with no glide, and recentring or changing the tracking room while moved through a room leaves `shift back` off by that much (the reset button puts the screens back).

What isn't there yet: a viewer can move their own copy of someone's panel until the room next moves it (who may move what is M4); the video's size is fixed when a screen is shared; with more than about eight remote displays the numbers would meet; Display Settings lists a peer's panel as a remote screen.

## Voices on a Frame

```
ft-roomvoice [--socket NAME] [--together NAME] [--screens NAME] [--muted] [--hum HZ] [--probe]
```

One program sends the wearer's voice to the room and plays the others'. It runs as a unit next to ft-together ("Running it").

- **Your voice.** The session's default microphone, which on the Frame runs through echo cancelling (it shows in the mixer as "Your voice in the room"), 20 ms at a time into Opus: mono, 24 kbit/s, with what the decoder needs to make up a lost packet from the next one. Silence isn't sent, but for a small packet every 400 ms that tells how the quiet sounds. Each 20 ms also moves your mouth: the five weights the mirror's mouth is made of (`avatar/face/face.h`) go to ft-together as they change (`voice/mouthsay.h`), and from there to the others in your poses, so the avatar you sent speaks when you do. A shut mouth is told once. Nothing of the voice is kept. The microphone is open only while you are in a room, not muted, and the headset is on your head: away in a game your voice stays on, with the headset off (`away off`) it doesn't.
- **Their voices.** Each person's packets wait about 60 ms in a buffer of their own (`voice/jitter.h`), so ones that come late or in a bunch still play in step. A lost one is made up from the next when that is there and guessed otherwise; after 100 ms of guessing with nothing waiting the voice stops until its next packet, and a voice more than 240 ms behind jumps to now. Packets are decoded here and nowhere else, and only ones that hold 20 ms.
- **Where they come from.** Each voice is heard from its owner's head (their pose's eyes, the same poses `ft-avatars --room` draws), by where your own head is on your floor (ft-screens' `head`, ten times a second) and where your floor lies in the room (`floor`). Further away is quieter, to a fifth at most, to the side is louder in that ear, and behind is a little quieter. It is loudness only, with no delay between the ears and no shaping by the head. Someone whose place isn't known, or anyone while your own head isn't, is heard in the middle. The mix shows in the mixer as "The room's voices".
- **Questions** on `@ft_roomvoice`: `mute`, `unmute`, `quit`, and `state`, which answers `ok` and JSON: `room`, `muted`, `mic` (`open`, `closed`, `failed`), `sent` and `level` (your voice), `playing`, and for each other person `person`, `speaking`, `placed`, `level`, `played`, `lost`, `late`.
- `--hum HZ` and `--probe` are for tests: the wearer hums a note in place of the microphone, and what would be played is measured (in `state`, as `out`) in place of the loudspeakers.

What isn't there yet: a volume for each person and for the room, the microphone's level made even, mute kept across runs or shown in the room, and how loud someone is shown on their figure. The mouth is told only while the microphone is open, so muted you are seen with your mouth shut. A steady tone is taken for noise by the encoder and not sent, which is right for a fan and wrong for a held note.

## Away

```
ft-roomaway [--together NAME] [--helper NAME] [--relay NAME]
```

While ft-together runs, `ft-roomaway` asks the pointer helper whether the headset is worn (`gaze ? headset`) and the input relay whether Frametop is paused for a game (`pause ?`), once a second in a room and every other second outside one, and tells ft-together `away off`, `away game` or `away back` whenever that has it otherwise. In a room, at each look where the helper's own answer has the headset worn and no game is on, it also says `control here`, which is what lets the wearer's grants stand ("Control of a shared panel"): being taken to be there for want of an answer is enough for the room, and not for letting others use the desktop. It tells outside a room too, so a room that is opened or joined with the headset off has its wearer away from the start, and no microphone opens for a headset that lies on a desk: a program here reads how its own wearer is away from `state`'s `away`, not from the room's copy of them, which trails it by a trip to the room's owner and back. The headset off wins over a game. An answer stands for five seconds; with none, the wearer is taken to be there, so a Frame without the pointer helper running isn't away for good. What follows from being away is each program's own: `ft-roomvoice` closes the microphone and plays nothing with the headset off, and a paused Frametop draws no screens, so a shared one stands still.

## Control of a shared panel

Everyone in a room sees everyone's pointer. Clicking and typing on someone's panel takes a grant from its owner, panel by panel and person by person, and every rule about it is kept in the owner's `ft-together` (`flow/control.h`), not in the guest's.

| Question on `@ft_together` | Answer |
|---|---|
| `control` | `ok` and, as JSON: `there` (whether someone says you have the headset on) and `left` (the seconds that word still stands), `asks` (who asks for which of your panels: `number`, `person`), `given` (who has which), `holder` (whose turn it is on your desktop, -1 nobody), `held` (the panels of others you were given: `panel`, and the seconds `left`), `asked` (the panels you asked for), and how many inputs were `passed` to your desktop and `dropped` |
| `control ask PANEL` | Asks the owner of the room's panel PANEL (its id) for it |
| `control give NUMBER PERSON` | Gives your panel NUMBER to the person in place PERSON, asked or not |
| `control take NUMBER [PERSON]` | Takes it back from them, or says no to their ask. Without PERSON, from everyone |
| `control act PANEL move X Y` | Your pointer on a panel you were given: across and down its picture, each 0 to 65535 |
| `control act PANEL button B 0\|1` | Button B (1 left, 2 right, 3 middle) let go or pressed |
| `control act PANEL scroll RIGHT DOWN` | A scroll, 120 a notch, 2400 at most |
| `control act PANEL key CODE 0\|1` | A key, by its Linux code, let go or pressed |
| `control own` | Your own hand is on your desktop: nobody else's input is taken for the next moment |
| `control here` | You are there, says the program that knows: what you gave stands ten seconds more |

**A grant is a lease.** It stands only while a program keeps saying the wearer is there, and ends ten seconds after the last word. `ft-roomaway` says `control here` once a second while the pointer helper says the headset is on your head and you aren't in a game ("Away"); with no helper answering, nobody says it, so nothing can be given and what was given runs out. A grant ends at once when you go away (`away game`, `away off`), take the panel back from the room, or leave, and when the person it was given to leaves. It is theirs, not their place's: someone else who comes into that place has none of it. An ask waits two minutes for an answer.

**One at a time.** A Frame has one desktop with one pointer and one keyboard, so the turn is the desktop's, not a panel's. Whoever's input comes while nobody has the turn takes it, and it frees a second and a half after their last input, or sixty seconds after it at most while they hold something down. Input from anyone else meanwhile is dropped. Your own hand takes the turn from anyone (`control own`, said by the program that sees your input).

**Nothing stays down.** ft-together remembers the buttons and keys each person holds, eight at most. When their turn, their grant or their link ends, it lets each go itself, the last pressed first, so a guest who vanishes mid-drag leaves no button down on your desktop.

**What may be typed.** Keys with Linux codes 1 to 111 except 99: the letters, digits, punctuation, modifiers, function, arrow and editing keys. Not the Meta keys, SysRq, or the power, sleep and volume keys. The check is the owner's; a guest's own `ft-together` refuses the same things first, to say why.

**What a grant gives away.** The panel names where the pointer goes. Keys go wherever the keyboard focus is on your desktop, which a click on the panel usually sets but which a shortcut can move. So giving someone a panel is letting them type on that desktop while you watch, and it is given only while you wear the headset for that reason.

What isn't there yet: asks and grants in Frametop Settings (until then `together/ft-room control ask PANEL` and `control give NUMBER PERSON` from a terminal). A panel that shows one of your PCs is to ask a second time.

### Your hand on someone's panel

Someone else's panel is a remote screen of ft-screens', shown by an `ft-peerpanel`, and ft-screens tells that program what your pointer and keyboard do on it in the words it tells a remote display's stream: `move X Y` in the picture's pixels, `button CODE 0|1`, `scroll DX DY` in notches, `key CODE 0|1` while the panel has the typing, and `blur` when typing goes elsewhere. A room's panel also hears `leave` when the pointer has left it: on SteamVR's own word for that, and whenever the pointer turns up on any other screen, because a 3D mouse or a gaze that moves on says none.

`ft-peerpanel` turns them into what `@ft_together` takes (`local/panelhand.h`, sent by `local/handsend.h`):

- **Where you point**, for everyone in the room: a pointer datagram with each move, sixty a second at most, with each button down or up, once a second while the pointer rests on the panel, and once more, pointing at nothing, when it leaves.
- **What you do**, for the panel's owner: a `control act` line for each move, button, scroll and key. Nothing here asks whether you were given the panel. Your own `ft-together` knows, and an act for a panel you weren't given goes nowhere.
- **Nothing stays down.** A button or key goes down once and comes up only if it went down through this program. Keys come up when typing goes elsewhere, buttons when the pointer leaves, and everything when the program ends. If `ft-together`'s queue is full, what didn't go waits and goes in order.

What isn't there yet: a click on a panel you weren't given still takes your typing to it, where it goes nowhere until you click elsewhere. Your pointer on your own shared screens isn't told to the room. The live program hasn't run with this: `ft-peerpanel` needs the hardware decoder, so its test is the same rules and sender without it (`tests/handwords.cpp`).

### The others' pointers

```
ft-roompoints [--socket NAME] [--together NAME] [--panels NAME] [--probe]
```

`ft-roompoints` draws where the others point. It asks `@ft_together` for their pointers (`points`, once a second) and `@ft_roompanels` which screen here shows each of the room's panels (`panels`, twice a second). While someone points at a panel this Frame shows, a small mark in the colour of their place in the room (the colour of their abstract body) is on it where they point: a ring, so what is pointed at shows through, and a whole disc while they hold a button down. It is 1.2% of the panel's width, between 1.2 and 4 cm.

- **Which panel.** Your own shared screen is ft-screens' panel for that screen, and someone else's panel is the remote screen it is decoded onto. The mark is a SteamVR overlay of this program's own, put on that panel's surface, flat or bent, 3 mm off it, and drawn over the panel and its controls. OpenVR's overlays can't be hung on one another, so the program reads where the panel is each time it draws (sixty times a second while a mark shows) and follows a panel that is moved a frame behind. A panel pinned to your view takes its marks with it. A panel that is hidden, or that this Frame doesn't show, has no mark.
- **How long.** A pointer rests with a word a second, so a mark stands 2.5 seconds after the last word and goes at once when its owner points at nothing. If `ft-roompanels` stops answering for three seconds, no panel is known and nothing is drawn.
- **Yours.** Your own pointer isn't drawn by this: ft-together sends a Frame only the others' pointers, and on your own screens the desktop's cursor is yours.
- **SteamVR.** It joins SteamVR as a background program when the first mark is wanted, so it never starts SteamVR or keeps it running, tries again every five seconds while SteamVR is down, and lets go when SteamVR quits. Each mark's picture is given to SteamVR once (SteamVR never frees what `SetOverlayRaw` takes), so each person has two overlays, the ring and the disc, and one of them shows.
- **Questions** on `@ft_roompoints`: `quit`, and `state`, which answers `ok` and JSON: `panels` (how many are known), `heard` (pointer datagrams taken), and `marks`, each with `person`, `panel`, `key` (the overlay's key, empty when the panel isn't shown here), `x` and `y` (0 to 1 across and down), `pressed` and `drawn`. With `--probe` nothing is drawn and SteamVR isn't joined, and `drawn` says what would be.

A mark says where someone points, not that they may click there: that is a grant's. What a peer sends becomes a disc of a fixed picture at a place bounded to the panel, and nothing else.

What isn't there yet: the marks have been checked in SteamVR by reading them back (below), not seen through the lenses. A pointer moves in steps of what its owner's Frame sends, up to sixty a second, without smoothing between them.

### To the desktop

```
ft-roominput [--together NAME] [--panels NAME] [--screens NAME]
```

`ft-roominput` is the program that takes the input (`inputs`, asked once a second) and gives it to the desktop. Each act becomes one command on `@ft_screens`, on the screen the panel is, which it learns from `ft-roompanels` (`state`):

| Command on `@ft_screens` | What ft-screens does |
|---|---|
| `peer <screen> move X Y` | Their pointer, 0 to 65535 across and down the screen, to the seat |
| `peer <screen> button B 0\|1` | A button, where they last moved to. A release only of what they pressed |
| `peer <screen> scroll RIGHT DOWN` | A scroll there, 120 a notch |
| `peer <screen> key CODE 0\|1` | A key. A press only while typing is that screen's; a release only of what they pressed |
| `peer letgo` | Every button and key they hold comes up |
| `peer on` | Someone has been given a panel: for the next five seconds ft-together hears `control own` when your own hand is on the desktop |

An act for a panel that is no screen of the desktop is dropped. `peer on` is said once a second while someone has a panel. `peer letgo` is said when ft-together says so (it missed telling an act), when nobody has a panel any more, when ft-together stops answering, and when ft-roominput ends.

The rules on the seat are ft-screens' own (`screens/peerinput.h`), because it is the one place that sees both people's input to the event:

- **Only the desktop's own screens.** Not a floating window's output, a remote display, Steam, or frunner's search. Nothing while Frametop is paused or Steam's PIN locks it.
- **What is theirs is kept apart.** Where their pointer is and what they hold is remembered for them, so a release reaches the seat only for a button or key they pressed, never one of yours, and their click lands where they point, wherever your pointer has been since. A key that is down already isn't pressed again for them.
- **Your hand takes the turn at once.** A click, a scroll, a drag, a spin to the front or a key of yours lets go of everything they hold before it reaches the desktop, and ft-together is told `control own`, at most twice a second.
- **Your resting pointer doesn't pull theirs.** While they hold a button, or were at the pointer in the last second and a half, a laser or gaze of yours that only rests on a screen or passes over it moves nothing: their drag isn't pulled to where you look.
- **Their click doesn't take your typing.** It takes the keyboard focus on the desktop to its screen, as any click does, but not the typing away from Steam, frunner or a remote display, and they type only while typing is the desktop's and on that screen.

What isn't there yet: all of this is tested against stand-ins and has not run on a live desktop (spike T6 is the headset test). While your pointer rests, the desktop's cursor stays where they left it. A side button of your mouse (Back) lands where the seat's pointer is, which may be theirs. The first notch of your wheel after they pointed at another screen can be lost.

## The pointer datagram

One datagram per person while their pointer moves, sixty a second at most, once a second while it rests on a panel, and one that points at none when it leaves; nine bytes: a `u8` kind (3), a `u16` count that tells a newer one from an older, the id of the shared panel they point at (255: at none), where on it as two `u16` (across and down its picture, 0 to 65535 at its far edges), and a flags byte (1: they hold a button down there). It says nothing of whether they may click there. A reader takes it only if the panel's id is one a room can have and no unknown flag is set.

## The voice datagram

One datagram per person for each 20 ms they are heard, at most 403 bytes: a `u8` kind (2), a `u16` count, one more for each 20 ms the sender's microphone gave whether sent or not, and one Opus packet of 1 to 400 bytes. The count puts the packets in order and tells how long a gap is. ft-together passes it between a link and this Frame's programs without reading the packet; whoever plays it hands it to the Opus decoder, which takes any bytes.

## The pose datagram

One datagram per person, about 30 times a second, at most 215 bytes: a `u8` kind (1), a `u16` count that tells a newer one from an older, the eyes' place, the head's turn, the eye height when standing, a flag nibble per hand, five mouth shapes, then for each tracked hand the wrist, its turn, fifteen finger bone turns when its fingers are tracked, and the elbow if the sender knows it. Places are in the room's coordinates. These are the inputs of the body solver ([avatar/README.md](../avatar/README.md)), not its result: every Frame runs the same solver on them.

A hand's turns belong to no body. The hand's turn is the frame of its palm, and each finger turn is the frame of that bone, all in the room's directions: which way the bone points and which way its back faces. The sender has them from its hand tracker, and each receiver fits them to the body it draws for that person, whose bones may rest differently from anyone else's. So the same datagram poses the abstract avatar and, once avatars are sent, the person's own.

A reader takes a datagram only if every place is within 100 m of the room's middle, every turn is close to unit length, the eye height is between 0.5 and 2.5 m, the flags make sense together and no byte is left over.

## Tests

`just test-c` builds these with the address and undefined-behaviour sanitizers and runs them. The links' test, the loopback test, ft-roompanels', ft-roomvoice's, ft-roomaway's, ft-roominput's and the hand's tests between two `ft-together` and control's and ft-roompoints' between three need msquic, which `together/build.sh` builds; without it they are SKIPPED. `tests/roomkit.py` is what the Python tests share: one `ft-together` on the loopback and the questions to it, a program on its media socket, a program whose lines are read as they come, a pose, a voice and a pointer datagram, an act's bytes, and a program that watches and listens.

| Test | What it covers |
|---|---|
| `tests/proto-test.cpp` | Every message and the pose datagram there and back, each cut short at every length, damaged, and made of noise; the stream parser fed in pieces of any size; panels alone and in a room |
| `tests/room-test.cpp` | Invites (once, ten minutes, eight waiting, the wrong-guess limit), spots, whose panels a room takes, and the allowance |
| `tests/session-test.cpp` | Up to five sessions run against each other on a network the test makes (`tests/fakenet.h`): joining, declining, a full room, the guests' links, leaving, a lost owner, another version, panels shared, moved and taken back, who is told who watches, the avatar each has on (before a room, changed, a lost word said again, a file asked for), and guests and strangers who say what they may not. Three hundred rounds of noise and forged messages on every kind of link |
| `tests/rejoin-test.cpp` | A headset that comes in again while the owner still holds its old link, on the same made network: it is in the room once, in one place, with one link at the owner, and the others are told. Two asks from one headset both let in, a second program at the same address, and someone who left and comes back |
| `tests/flow-test.cpp` | The video's small rules: who gets the next picture, the order a viewer decodes them in, the bitrate, and a picture's bytes on its stream |
| `tests/video-test.cpp` | Three sessions on the test's network, each with the video's rules, a wire the test can lose, hold back and refuse pictures on, and programs that remember what they were told: who is sent what and when, keyframes asked for, pictures out of order, the bitrate, a guest's panel over the guests' own link, and pictures nobody may send |
| `tests/encsize-test.cpp` | The sizes the Frame's encoder is given (`media/irisenc.h`, `Takes`): widths in steps of 128 only, even heights, and `Open` refusing a size before it touches the device (a device that is `/dev/null` is reached only by a size it takes) |
| `tests/controlrules-test.cpp` | Control's small rules: the pointer datagram and control's messages there and back, cut short and out of bounds; the pointer gate and ft-together's own count; grants (asked, given only while the wearer is there, running out, taken back, an ask that waits); and the desk (one at a time, the hold, what is held down and let go, the wearer's own hand, the keys a guest may type) |
| `tests/control-test.cpp` | Four sessions on the test's network, each with control's rules behind its lines of text and a desktop that remembers its input: asking, giving and taking back; input in order and as whose it is; a grant for one panel and one person; one at a time; a held button let go on taking back, on the wearer's own hand, when the lease runs out, when the link is lost; a late word that the wearer is there; away; a panel taken back; a guest's panel over the guests' own link; someone new in a leaver's place; how fast the wearer's own programs may send; and people who say what they may not, too much, or as strangers |
| `tests/looks-test.cpp` | Three sessions on the test's network, each with the avatars' rules, files the test carries, and a closet that remembers: a file asked for once and from its wearer only, kept when its hash and size are the room's, taken off, changed, asked for again when it didn't come, gone on leaving; and files nobody asked for, from the wrong person, of the wrong size or content, asked for too often, or asked of someone who doesn't have them on |
| `tests/wardrobe-test.cpp` | The avatars' files on a Frame, in folders the test makes: which model the room may hear of (marked, unmarked, missing, not a model, too large, a name that isn't one), a file read again only when it changed, and the files that came: kept whole in a private folder, four at most, gone when unworn and when the program ends, an earlier run's leftovers cleared, and nothing kept in a folder that is a link or without a session folder |
| `tests/tailnet-test.cpp` | tailscaled's answers, kept in the test: this node's address and name, who is at an address, Tailscale stopped or logged out, answers that aren't what they should be; the JSON reader on good texts, nearly good ones and noise; which addresses are on the tailnet; a name looked up |
| `tests/local-test.cpp` | The pose gate (what passes, an older pose, a flood, a link forgotten), ft-together's own count, the wearer's mouth (in their poses while it is told, shut half a second after, what isn't one), the voice datagram and its gate (the sizes it takes, an older one, the count going round, a flood), the watchers and the listeners (four at most, forgotten after five seconds, what each is sent, how many missed it), the others' input to the one program that takes it (as `0x85`, in order, a let-go after a miss, another program in its place), the floor (the first said stands, lifted on leaving, moved only once laid), a line of text told to those who watch, and the media socket's messages however the socket cuts them up |
| `tests/links-test.cpp` | Real links between ends in one program, on loopback: bytes in order both ways, datagrams, closing, calls that may not be made or that nobody answers, a call to another address of this machine, the limits on what a far end can make an end hold, and taking an end down with links up. Pictures: whole either way, the longest, a burst, untaken ones bounded, and on a network that stops carrying, each given up after 150 ms and told, never arriving late. Files: one that wasn't asked for isn't taken, one that was arrives whole and once, a longer one than asked for is refused, the largest both ways beside pictures and datagrams, a link closed or an end taken down with files on their way, and 300 files started on a link that is busy with datagrams both ways (starting one used to wait for msquic's thread with the lock held, which hung now and then). A run that is stuck fails after 15 minutes |
| `tests/whereabouts-test.cpp` | Where the others were last seen: their eyes and the way they face from a pose, level whatever they look up or down at, each in the order of their places, forgotten after two seconds, a place nobody can have |
| `tests/loopback-test.py` | Four `ft-together --loopback` programs: a room opened, a wrong code, a declined ask, noise on the port, two guests let in and linked to each other, away, a guest whose program is killed, leaving and coming back, a second program on the same socket, and the owner's program ending. Poses: one handed to a program reaches the watchers of the others' programs with whose it is and that program's own count, from the owner and from a guest, and nothing reaches a program nobody watches, a pose's own sender, or anyone after leaving; each program keeps its own floor, a moved floor stands and is told at once to the programs that watch and listen and to nobody else's, and `where` says where a guest's pose put them until they stop sending. A mouth told to a program goes out in its wearer's poses and in nobody else's, and shuts when it isn't told again. Voices: one handed to a program reaches the others' listeners as it was sent, with whose it is, through the count going round; a late one, one that isn't a voice, and a person's own never do. Avatars: a model the owner or a guest has on comes to the others as a file, whole (up to two megabytes here) and where only they can read it, a newcomer gets it too, an unmarked one stays private, and the files go when it is taken off, on leaving, and when the program ends. Panels: shared by the owner and by a guest, in everyone's room, gone with their owner. Video: test programs on the media sockets stand in for an encoder and two viewers' programs, and pictures cross real links in order and byte for byte, up to a megabyte; a second viewer waits for its keyframe; nobody watching is told to the encoder; and programs that say what they may not, say nothing, or don't take their pictures are let go |
| `tests/test_roompanels.py` | ft-roompanels' rules without either program (`just test-python`): the floor's sums there and back, a guest across the room seeing a panel's back; the keeper against stand-ins that answer as ft-together and ft-screens do: the floor laid from the head or without one, a peer's panel made, placed once, moved with the room, gone with it, made again at another video size, remote displays left alone, hostile names, what an earlier run left; a screen shared, moved, taken back, three at most; which screen here shows each of the room's panels (`panels`), mine as large as the desktop has it, theirs once placed, none before the room has it or after it went; leaving and ending, with the screens put back where they stood; ft-together silent for a moment |
| `tests/test_roommoving.py` | Moving yourself without either program (`just test-python`): the sums (a floor after a shift, the shift between two floors, the spot beside someone and the one in front of a panel, upright, leaning or flat); `go` to a place, a person and a panel puts the head there and leaves the room's things where they are in the room; `pull`; what is refused (no head, no floor, yourself, nobody there, too far, more than a pull, rubbish) and that a floor that won't move undoes the shift; with the keeper beside it, a panel of someone's isn't placed again and a shared screen isn't told again |
| `tests/roompanels-test.py` | `ft-roompanels` itself between two real `ft-together --loopback`, each with a stand-in ft-screens on a socket (`tests/fakescreens.py`): each floor laid, a screen shared by the owner and one by the guest standing at the other's where the room says (each side's pose taken back to the room through its own floor), what `panels` says at each, moved, taken back, gone when its owner leaves, and nothing left behind when ft-roompanels ends. Moving: the guest goes to a panel, to the owner (found by `where` from a real pose) and pulls, and after each the shared screen is where it was in the room and wasn't placed again; leaving puts their screens back |
| `tests/voice-test.cpp` | The voices' parts without a sound device: the buffer (in step, late, in a bunch, lost, a pause, a stall, the count going round, a voice far behind), where a voice is heard from, Opus there and back (the note that went in comes out, silence isn't sent, a lost packet made up, four thousand packets of noise, a packet that isn't 20 ms), the mix (two voices, each in its ear, never past full scale, a person forgotten), and the wearer's mouth from their voice (told while they speak with the weights the face's sums make, shut once, and afresh when the microphone opens again) |
| `tests/roomvoice-test.py` | `ft-roomvoice` itself, twice, between two real `ft-together --loopback`, each with a stand-in ft-screens, humming a note of its own and measuring what it would play: nothing outside a room, one's note heard by the other and not by themselves, from the side its owner stands on and quieter from further off, in the middle when their place isn't known, mute and unmute, the headset off and a game, leaving, and ending cleanly. One's mouth is open in the poses the other gets while they hum, and shut while they are muted |
| `tests/test_roomaway.py` | ft-roomaway's rules (`just test-python`): the helper's and the relay's answers read, good and not; the headset off, a game, both, back; what the room has put right; an answer that stands five seconds; told outside a room too, and again to an `ft-together` that started again |
| `tests/roomaway-test.py` | `ft-roomaway` itself next to a real `ft-together` in a room with another, with stand-ins for the pointer helper and the relay: off, a game and back reach the other end's room, what another program said is put right, a helper that stops answering, leaving, and ending cleanly. That the wearer is there reaches their own `ft-together` while the helper says so, and stops when it stops answering |
| `tests/roomcontrol-test.py` | Three real `ft-together --loopback`: one program takes a desktop's input and a second is told no; a panel asked for, not given until the wearer is said to be there, then given; the guest's moves, clicks, keys and scrolls reach that program in order as theirs, and nobody else's do; a held button let go when the panel is taken back and when its holder leaves; a grant that runs out by the real clock about ten seconds after the last word, with the held key let go; and pointers from one program to the others' with ft-together's own count, and what isn't one |
| `tests/panelhand-test.cpp` | Your hand on someone's panel, its rules alone: moves across and down the picture, past its edges and before its size is known; sixty moves a second at most, the last one sent when it is due or before the click or scroll that happens there; the three buttons, down once and up only if down; scrolls in 120ths and bounded; keys, eight at most, up when typing goes elsewhere; a resting pointer said again each second, a leaving one once; the end; and twenty thousand words of any kind, after which every line out is input a panel takes and nothing is left down |
| `tests/handsend-test.cpp` | The sender of what your hand says (`local/handsend.h`), against a socket that reads only when the test says so: into a full queue nothing goes and the call comes back; when there is room, the oldest go first and the rest wait in order; it all arrives once each as the queue empties; of more than 256 waiting the oldest are dropped; said to where nobody listens, it is dropped |
| `tests/panelhand-test.py` | The same rules and sender in a program without the decoder (`tests/handwords.cpp`), between two real `ft-together --loopback`: without a grant the other end sees the pointer and its desktop gets nothing; given the panel, moves, a drag, keys and scrolls reach the program at the owner's desktop in order; a resting pointer and one that leaves; and a program that ends with a button and a key down |
| `tests/test_roominput.py` | ft-roominput's rules (`just test-python`): ft-together's and ft-roompanels' answers and an act's bytes read, good and not; nothing said while nobody has a panel; `peer on` each look while someone has; an act on the screen its panel is; a let-go; what is no act or for no screen dropped; a panel given since the last look asked about at once, not too often; and everything let go once when the grants end, when ft-together stops answering and at the end |
| `tests/roominput-test.py` | `ft-roominput` itself next to a real `ft-together` in a room with another, with stand-ins for ft-roompanels and ft-screens: a second one is told no; a guest's moves, clicks, keys and scrolls reach ft-screens in order on the right screen; `control own` lets go of a held button and holds their input off for a moment; a panel that is no screen goes nowhere until it is one; a key let go when the panel is taken back, then everything; and everything let go when ft-together goes and when it ends |
| `screens/tests/peerinput-test.c` | ft-screens' side (`just test-c`, with the sanitizers), against a desk that writes down what it is asked: their pointer in the screen's pixels, buttons where they point, releases only of what they hold, keys only where typing is that screen's, eight keys at most, letting go in order, a screen that went, what isn't a command, whose turn the pointer is, and when ft-together is told of the wearer's hand |
| `tests/points-test.cpp` | ft-roompoints' rules: a pointer's word taken and whose it is, how long it stands, pointing at nothing, what isn't a pointer refused (a place or panel nobody can have, a short one, a long one, text); ft-roompanels' answer read whole or not at all, with every way it can be wrong; a picture's point on a flat panel, a tall one, a bent one (on the cylinder, lifted toward its middle line, facing it), a panel that stands somewhere turned; the mark's size; and its picture (ring and disc, the colour, the rim, round) |
| `tests/roompoints-test.py` | `ft-roompoints --probe` itself at one of three real `ft-together --loopback`, with a stand-in for ft-roompanels: a second one is told it runs already; where one guest points is a mark of theirs on the owner's screen, another's with a button held on the remote screen that shows a guest's panel; a mark moves with its pointer; the owner's own pointer is no mark; a panel not shown here is told and not drawn; pointing at nothing, and silence; ft-roompanels gone quiet, back with another screen, and saying what isn't an answer; `quit`. It needs OpenVR's header and library to build the program, and is SKIPPED without them |
| `tests/test_roompage.py` | The Settings page (`just test-python`): the answers read, good and not; what is typed to join; and the backend and the page itself driven against stand-ins for the four programs on sockets of the test's own (`tests/pagekit.py`), with a made-up systemctl: the switch, a room opened, an invite, an ask let in, joining, mute, a screen shared, and no QML warnings |
| `tests/test_roomavatar.py` | The page's avatar row: what the worn model's file and `looks` say read into the row; the file read once until it changes; nothing marked before a yes, nor after a cancel; a command that fails; and the built `ft-model-import` marking a model it imported, from the page |
| `tests/test_units.py` | The units as files: the room's six go with ft-together's and are never enabled themselves, each starts a program there is, each cleans up only its own, and `run.sh` and `install.sh` name them all |
| `screens/tests/share-test.c` | ft-screens' share tap with made-up buffers, the test being the `ft-sharepanel` it starts: the commands, each buffer described once, one picture out at a time and the newest next, the rate, a program that keeps a picture or dies, pictures only while watched |
| `screens/tests/remote-test.c` | With the remote displays' cases: a room's panel is given its id and the room's socket, waits for an old program of its own screen and for no host's queue, and what isn't one is refused; and it hears once that the pointer left it, whichever way that shows, which a remote display's stream never hears |
| `tests/panels-test.py` | **Run by hand, not by `just test`**: it takes the Frame's encoder and decoder and checks the rate the pictures come at, which a loaded suite would fail (`together/tests/panels-test.py together/build/ft-together together/build/ft-sharepanel together/build/ft-peerpanel`; all 23 checks pass, 2026-11-11). The real video: `ft-sharepanel --pattern` to `ft-peerpanel --probe` through two `ft-together`, checking every picture's size, number and colours, the encoder closing when nobody watches, a viewer coming back, the sharer starting again, a width the encoder isn't given being refused, and six sizes. It needs the Frame's encoder and decoder |
