# Frametop Together

Shared rooms: up to four people in one owner's room, each on their own Frame, over Tailscale. The design, its decisions and the milestones are in [docs/together.md](../docs/together.md). This file says what is in this folder and what goes over a link.

Built so far: the spikes (M0), and the session's wire format and rules with their tests (the first part of M3). The links themselves, `ft-together`, panels and voice are not built yet.

## What is where

| Folder | What it holds | Depends on |
|---|---|---|
| `proto/` | The bytes on a link: bounded readers and writers (`bytes.h`), the control stream's messages (`messages.h`), the pose datagram (`posewire.h`). No I/O, standard library only, so the PC sender in the Vibepollo fork can take the folder as it is | nothing |
| `room/` | A room's own rules: invites (`invites.h`) and where people stand (`seats.h`) | `proto/` |
| `session/` | The session: who may join, who is in the room, who links to whom, and what each link may say (`session.h`). The network, the tailnet, the clock and the dice are handed in | `proto/`, `room/` |
| `net/` | The real network: a self-signed certificate for QUIC (`selfcert.h`). msquic's links and the tailnet through tailscaled come next | msquic, OpenSSL |
| `media/` | The iris hardware encoder (`irisenc.h`) | V4L2 |
| `spike/` | The M0 spikes, `ft-enctest` and `ft-quictest` | `media/`, `net/` |
| `tests/` | The tests below | |

`together/build.sh --spikes` builds the spikes and msquic (pinned by commit) in the dev container.

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

One datagram per person, about 30 times a second, at most 215 bytes: a `u8` kind (1), a `u16` count that tells a newer one from an older, the eyes' place, the head's turn, the eye height when standing, a flag nibble per hand, five mouth shapes, then for each tracked hand the wrist, its turn, fifteen finger joint turns when its fingers are tracked, and the elbow if the sender knows it. Places are in the room's coordinates. These are the inputs of the body solver ([avatar/README.md](../avatar/README.md)), not its result: every Frame runs the same solver on them.

A reader takes a datagram only if every place is within 100 m of the room's middle, every turn is close to unit length, the eye height is between 0.5 and 2.5 m, the flags make sense together and no byte is left over.

## Tests

`just test-c` builds these with the address and undefined-behaviour sanitizers and runs them.

| Test | What it covers |
|---|---|
| `tests/proto-test.cpp` | Every message and the pose datagram there and back, each cut short at every length, damaged, and made of noise; the stream parser fed in pieces of any size |
| `tests/room-test.cpp` | Invites (once, ten minutes, eight waiting, the wrong-guess limit) and spots |
| `tests/session-test.cpp` | Up to five sessions run against each other on a network the test makes: joining, declining, a full room, the guests' links, leaving, a lost owner, another version, and guests and strangers who say what they may not. Three hundred rounds of noise and forged messages on every kind of link |
