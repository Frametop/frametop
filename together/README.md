# Frametop Together

Shared rooms: up to four people in one owner's room, each on their own Frame, over Tailscale. The design, its decisions and the milestones are in [docs/together.md](../docs/together.md). This file says what is in this folder and what goes over a link.

Built so far: the spikes (M0), and from M3 the session's wire format and rules, the links, `ft-together`, which opens a room, lets people in and keeps the room's people linked, and poses: each person's head and hands go to the others, and `ft-avatars --room` ([avatar/README.md](../avatar/README.md)) draws them as abstract avatars. Panels and voice are not built yet, so people in a room see each other but not each other's screens, and don't hear each other so far.

## What is where

| Folder | What it holds | Depends on |
|---|---|---|
| `proto/` | The bytes on a link: bounded readers and writers (`bytes.h`), the control stream's messages (`messages.h`), the pose datagram (`posewire.h`). No I/O, standard library only, so the PC sender in the Vibepollo fork can take the folder as it is | nothing |
| `room/` | A room's own rules: invites (`invites.h`) and where people stand (`seats.h`) | `proto/` |
| `session/` | The session: who may join, who is in the room, who links to whom, and what each link may say (`session.h`). The network, the tailnet, the clock and the dice are handed in. `commands.h` puts a session behind lines of text | `proto/`, `room/` |
| `local/` | What ft-together does for this Frame's own programs beyond the session's commands: the poses it lets through each way (`posegate.h`), who gets the others' poses (`watchers.h`), where this Frame's floor lies in the room (`floor.h`), and the datagrams on the local socket that aren't text (`datagrams.h`) | `proto/` |
| `net/` | The real network: the links on msquic (`quiclinks.h`), with the self-signed certificate QUIC wants (`selfcert.h`), which address a name stands for (`lookup.h`), and who is at an address by tailscaled's word (`tailnet.h`, with the small JSON reader for its answers, `json.h`) | `session/`, msquic, OpenSSL |
| `ft-together.cpp` | The program: one loop over the links and a local socket | `session/`, `net/`, `local/` |
| `media/` | The iris hardware encoder (`irisenc.h`) | V4L2 |
| `spike/` | The M0 spikes, `ft-enctest` and `ft-quictest` | `media/`, `net/` |
| `tests/` | The tests below | |

`together/build.sh` builds `build/ft-together` and msquic (pinned by commit) in the dev container, and with `--spikes` the spikes too.

## ft-together

```
ft-together [--port N] [--name NAME] [--socket NAME] [--loopback]
```

It runs while Together is wanted. It asks tailscaled for this Frame's tailnet address and name, takes calls on that address (port 47193 unless `--port` says otherwise), and holds the session. While Tailscale isn't running it waits and looks again every three seconds. `--name` is what the others call you; without it, the headset's name on the tailnet.

The rest of Frametop talks to it on `@ft_together`, one datagram a question and one back, like the other Frametop sockets:

| Question | Answer |
|---|---|
| `state` | `ok` and the session as JSON: `mode` (`idle`, `hosting`, `calling`, `joined`), `note` (the last thing worth telling the wearer), `me`, `owner`, `people` (place, name, login, away, whether there is a link to them, spot), `asks` (who waits for the owner's answer: number, name, Tailscale login, address) and `codes` (invites nobody used yet) |
| `open [X Z YAW]` | A room, with you its owner standing there |
| `invite` | `ok` and a code |
| `join HOST PORT CODE` | Calls a room's owner. `state` tells how it went |
| `accept N`, `decline N` | An ask answered |
| `away game`, `away off`, `away back` | Tells the room |
| `leave` | Leaves the room. Its owner leaving ends it for everyone |
| `watch` | `ok`, and for the next five seconds every pose a person in the room sends comes to the asker (below). A program that draws the room asks again each second. At most four watch at once |
| `floor` | Where this Frame's own floor lies in the room: `ok X Z YAW`, or `ok none` while no program has said |
| `floor X Z YAW` | Says where it lies. The first said stands until you leave the room, and the answer is the one that stands |

Two kinds of datagram on the socket aren't text, and are told by their first byte, which no text starts with:

- **Your own pose**, to ft-together: a pose datagram as it goes on a link (first byte 1, below), in the room's directions. ft-together reads it as it reads a peer's, puts its own count in it and sends it to everyone in the room it has a link to. It isn't answered, and outside a room it goes nowhere.
- **Someone's pose**, to each watcher: the byte `0x81`, their place in the room (a byte), then their pose datagram. Only a pose from a link whose person is in the room is passed on, only if it reads, is newer than that link's last, and is within 90 datagrams a second from that link (three times what a Frame sends), rubbish counted.

**Your floor in the room.** Everyone tracks themselves on their own floor, and the room has its owner's directions. A place on your floor, turned by YAW radians about the way up and then moved by X and Z metres, is that place in the room. It is laid once, when you come in, so that where you stand then is the spot the room gave you, facing the way the spot faces. Every program of yours that draws the room or tells it where you are needs the same answer, so ft-together keeps it: the first program that knows where you stood says it, and the rest ask.

Anything that can't be done is answered with `error` and why. Ending the program (SIGTERM) leaves the room first, so the others are told at once.

```
echo -n state | socat -t 1 - ABSTRACT-SENDTO:ft_together,bind=
```

`--loopback` is for tests: no Tailscale, the links are on 127.0.0.1 only, and everyone there is let in as `someone@loopback`. A fake peer for the live Frame is a second `ft-together` in the normal mode on another port and socket: it has this Frame's tailnet address, and the session tells people apart by address and port.

## The links

One QUIC connection for every two people, with one stream for the control messages, opened by the caller, and datagrams beside it for poses and voice (at most 1187 bytes each, what a link over Tailscale carries). msquic runs threads of its own; what they see is queued, and `ft-together`'s one loop takes it out and tells the session.

- The listener takes calls sent to this Frame's tailnet address only. Who a caller is comes from tailscaled, asked about the address the call came from. The certificate QUIC wants is made new at each start, names nobody, and isn't checked: Tailscale already encrypts the path and vouches for the address at its other end.
- A name is looked up before a call, and only an address on the tailnet is ever sent a packet.
- An end holds at most 32 connections. A link whose bytes pile up untaken past 1 MB is cut, and datagrams past 256 KB untaken are dropped: the session reads faster than anyone has reason to write.
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
| 4 Room | room id, turn, owner's place, 1 to 4 people: place, name, login, address, port, away, spot | The owner, to every guest, whenever the room changes. `turn` counts up; an older one is passed over |
| 5 Away | playing a game, headset off, or back | A guest to the owner, who puts it in the room |
| 6 Leave | nothing | Anyone, before hanging up. From the owner it ends the room |

A Hello of another version is read no further than its version, and answered with Refuse (why 1) and a sentence asking to update. Kinds a reader doesn't know are skipped when they come from someone in the room, so a later version can add messages.

A link that says anything it may not is closed: a stranger who says anything but Hello, a second Hello, a Room or Welcome from anyone but the owner, a message that doesn't read. A link gets ten seconds to say Hello, and at most eight links wait to at once.

## The pose datagram

One datagram per person, about 30 times a second, at most 215 bytes: a `u8` kind (1), a `u16` count that tells a newer one from an older, the eyes' place, the head's turn, the eye height when standing, a flag nibble per hand, five mouth shapes, then for each tracked hand the wrist, its turn, fifteen finger bone turns when its fingers are tracked, and the elbow if the sender knows it. Places are in the room's coordinates. These are the inputs of the body solver ([avatar/README.md](../avatar/README.md)), not its result: every Frame runs the same solver on them.

A hand's turns belong to no body. The hand's turn is the frame of its palm, and each finger turn is the frame of that bone, all in the room's directions: which way the bone points and which way its back faces. The sender has them from its hand tracker, and each receiver fits them to the body it draws for that person, whose bones may rest differently from anyone else's. So the same datagram poses the abstract avatar and, once avatars are sent, the person's own.

A reader takes a datagram only if every place is within 100 m of the room's middle, every turn is close to unit length, the eye height is between 0.5 and 2.5 m, the flags make sense together and no byte is left over.

## Tests

`just test-c` builds these with the address and undefined-behaviour sanitizers and runs them. The links' test and the loopback test need msquic, which `together/build.sh` builds; without it they are SKIPPED.

| Test | What it covers |
|---|---|
| `tests/proto-test.cpp` | Every message and the pose datagram there and back, each cut short at every length, damaged, and made of noise; the stream parser fed in pieces of any size |
| `tests/room-test.cpp` | Invites (once, ten minutes, eight waiting, the wrong-guess limit) and spots |
| `tests/session-test.cpp` | Up to five sessions run against each other on a network the test makes: joining, declining, a full room, the guests' links, leaving, a lost owner, another version, and guests and strangers who say what they may not. Three hundred rounds of noise and forged messages on every kind of link |
| `tests/tailnet-test.cpp` | tailscaled's answers, kept in the test: this node's address and name, who is at an address, Tailscale stopped or logged out, answers that aren't what they should be; the JSON reader on good texts, nearly good ones and noise; which addresses are on the tailnet; a name looked up |
| `tests/local-test.cpp` | The pose gate (what passes, an older pose, a flood, a link forgotten), ft-together's own count, the watchers (four at most, forgotten after five seconds, what each is sent) and the floor (the first said stands, lifted on leaving) |
| `tests/links-test.cpp` | Real links between ends in one program, on loopback: bytes in order both ways, datagrams, closing, calls that may not be made or that nobody answers, a call to another address of this machine, the limits on what a far end can make an end hold, and taking an end down with links up |
| `tests/loopback-test.py` | Four `ft-together --loopback` programs: a room opened, a wrong code, a declined ask, noise on the port, two guests let in and linked to each other, away, a guest whose program is killed, leaving and coming back, a second program on the same socket, and the owner's program ending. Poses: one handed to a program reaches the watchers of the others' programs with whose it is and that program's own count, from the owner and from a guest, and nothing reaches a program nobody watches, a pose's own sender, or anyone after leaving; each program keeps its own floor |
