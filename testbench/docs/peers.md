# Fake peers for Frametop Together

A fake peer is someone in a Frametop Together room who isn't there. It is Frametop's own programs
with nobody wearing them: an `ft-together` of its own (the real session code, on a port and a
socket of its own), `ft-avatars --pretend` standing in for a tracked wearer, and
`ft-roomvoice --hum` for their voice. Up to three run at once, which fills a room of four with
the Frame's wearer. Everything the Frame's own programs do with a real guest they do with a fake
one: the ask on the Frametop Together page, the links, the poses, the avatar, the voice and its
mouth, away. See [docs/together.md](../../docs/together.md) for the design and
[together/README.md](../../together/README.md) for the programs.

```sh
./frame-testbench peer 1 start --name Bo      # runs it; it stands in no room yet
./frame-testbench peer 1 join                 # into the room open on this Frame
./frame-testbench peer 1 pose wave            # stand, wave, reach, type, sit, lean, look
./frame-testbench peer 1 walk 1 0.5 90        # 1 m to its right, 0.5 m back, turned 90 degrees left
./frame-testbench peer 1 speak 220            # hums a note of 220 Hz: its mouth moves too
./frame-testbench peer 1 speak off
./frame-testbench peer 1 wear Robot           # a model of the avatars folder, by name
./frame-testbench peer 1 wear                 # none: the abstract figure
./frame-testbench peer 1 away game            # in a game; off: its headset off; back
./frame-testbench peer 1 state
./frame-testbench peer 1 leave                # out of the room, still running
./frame-testbench peer 1 stop

./frame-testbench release                     # also stops every fake peer
```

`walk` is measured from the spot the room gave the peer, and isn't added to an earlier walk. A
negative number needs `--` before the numbers (`walk -- -1 0 0`).

## Joining

`peer N join` brings the peer into the room this Frame's wearer has open. The testbench does what
the wearer would: it asks the Frame's `ft-together` for an invite code, has the peer call with
it, and accepts the ask that carries the peer's name and this Frame's address. It accepts no
other ask, and it never opens a room: Frametop Together must be on and the wearer's room open
(the Frametop Together page, or `together/ft-room open`). A peer that is refused says why (the
room is full, for one).

`peer N join HOST PORT CODE` calls another room instead, with a code its owner made. Its owner
answers the ask.

## How it works

`peer N start` runs `ft-peer-sim N` as the transient user unit `frame-testbench-peer-N`
(`journalctl --user -u frame-testbench-peer-N` for its log), inside the container Frametop's
programs were built in (`scripts/in-box`), so it can start and end them. It needs
`together/build.sh` and `screens/build.sh` to have run there.

| | Peer N |
| --- | --- |
| Takes calls on | this Frame's tailnet address, port 47193 + N (the Frame's own `ft-together` has 47193) |
| Its `ft-together`'s socket | `@ft_peer_N` |
| Its own socket | `@ft_peer_sim_N`, one datagram a question and one back |
| Name | `Peer N`, or `--name` |

`ft-peer-sim` checks each question before it passes anything on (a pose is one of the seven, a
walk is within 20 m and a turn within 360 degrees, a note is 50 to 2000 Hz, a model's name has
no path in it), and reads nothing a room sends: the programs it starts do, by the session's own
rules. `stop` asks it to end over its socket, since a signal to the unit doesn't reach a program
in the container; it then leaves its room and ends its programs. One that doesn't end in 15
seconds is told with SIGTERM, and the command says so.

A fake peer listens where a real guest's Frame would: on the tailnet, behind the same rules as
the Frame's own `ft-together` (a caller needs an open room there and a code, and a fake peer
never opens a room). `start --loopback` keeps its links on 127.0.0.1, for a room whose owner
also runs with `--loopback`; the tests use it.

What is real: the session, the links (QUIC between the two `ft-together` on this Frame), the
room's rules, the pose datagrams, the Opus voice and its mouth shapes, an avatar's file crossing
a link. What isn't: the wearer (a pose that plays, not a tracked head and hands), the voice (a
hummed note, with no microphone opened and nothing played from the peer's end), and the network
(both ends are on this Frame, so there is no delay or loss). While a peer is away nobody stands
there: its pretended wearer is ended until it is back, as `ft-avatars --room` tells the room
nothing of a wearer who is away.

## Models

`wear NAME` has the peer wear a model of this Frame's avatars folder
(`~/.local/share/frametop/avatars`, or the folder `start --models` names). The others get its
file only if it is marked as one to pass on, as for a real wearer
([avatar/README.md](../../avatar/README.md)); otherwise they see the abstract figure, and
`state` says why in `wear`.

## What it can't do yet

- Share a panel. A fake peer's panel would run this Frame's hardware decoder, which is held
  back for now ([docs/together.md](../../docs/together.md), "Open questions").
- Point at a panel, click, or ask for control (those come with the session's pointers).
- Speak from a recording. It hums.
- A slow or lossy link: no delay, loss or rate cap between it and the Frame.
- Run on another computer and call over Tailscale.

## MCP tools

`peer_start`, `peer_stop`, `peer_state`, `peer_join`, `peer_leave`, `peer_pose`, `peer_walk`,
`peer_avatar`, `peer_speak` and `peer_away` ([mcp.md](mcp.md)). The tools join only the room
open on this Frame, and take no host, port, code, socket or folder.

## Tests

`tests/test_peersim.py` checks the simulator's rules with stand-in programs and, where
Frametop Together is built, runs a real `ft-peer-sim` into the room of an
`ft-together --loopback`: it is let in, stands, walks a metre, hums with its mouth moving, goes
away and leaves, and no program is left behind. `tests/test_peers.py` checks the client and the
`peer` commands against stand-ins on the sockets, and once for real: a peer started, let into a
room, told to wave and stopped. `tests/test_mcp_peers.py` checks each tool's command and what
the tools refuse (it needs the MCP SDK, like the other MCP tests). Seeing and hearing a fake
peer in the headset is a headset test.
