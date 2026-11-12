# Fake peers for Frametop Together

A fake peer is someone in a Frametop Together room who isn't there. It is Frametop's own programs
with nobody wearing them: an `ft-together` of its own (the real session code, on a port and a
socket of its own), `ft-avatars --pretend` standing in for a tracked wearer,
`ft-roomvoice --hum` for their voice, `ft-sharepanel --pattern` for a screen they share, and
`ft-pcsend` for their split compute PC. Up to three run at once, which fills a room of four with
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
./frame-testbench peer 1 point 0 0.25 0.75    # its pointer on panel 0, a quarter across and three down
./frame-testbench peer 1 look 20 -10          # shares where it looks: 20 degrees to its left, 10 down
./frame-testbench peer 1 ask 0                # asks panel 0's owner for it
./frame-testbench peer 1 act 0 button 1 1     # on a panel it was given: the left button down
./frame-testbench peer 1 put 8 0 1.5 -2 90    # puts panel 8 there in the room, turned 90 degrees left
./frame-testbench peer 1 share                # a test panel of its own in the room
./frame-testbench peer 1 give 0               # gives its panel to the person in place 0, the room's owner
./frame-testbench peer 1 take                 # takes it back from everyone
./frame-testbench peer 1 unshare
./frame-testbench peer 1 pc 4                 # its split compute PC comes, with four windows in the room
./frame-testbench peer 1 unpc
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

## Its hand and its eyes

A panel is named by its id, as `peer N state` lists the room's panels under `together`: the
place of its owner in the room times eight, plus its number among that person's panels. The
first screen the room's owner shares is panel 0.

| Command | What the peer does |
| --- | --- |
| `point PANEL X Y [--down]` | Its pointer is on that panel, X across and Y down the picture, each 0 to 1, with a button held when `--down`. Everyone in the room is told, and told again each second while it stays there |
| `point off` | Its pointer is on no panel |
| `look YAW PITCH` | It shares where it looks: degrees to its left and up from the way its head faces, 63.5 at most each way. The direction goes out in its poses until `look off` |
| `look off` | It shares that no longer |
| `ask PANEL` | It asks the panel's owner for control of it |
| `act PANEL move X Y` | On a panel it was given: its pointer on the owner's desktop goes there, each 0 to 1 |
| `act PANEL button B 0\|1` | Button B (1 left, 2 right, 3 middle) let go or pressed |
| `act PANEL scroll RIGHT DOWN` | A scroll, 120 a notch, 2400 at most each way |
| `act PANEL key CODE 0\|1` | A key by its Linux code (1 to 111, not 99; 30 is A), let go or pressed |
| `put PANEL X Y Z [DEGREES]` | It puts someone else's panel there: its middle in metres in the room, turned that much to its left |

Each becomes what a Frame's own programs say to their `ft-together`
([together/README.md](../../together/README.md): the pointer datagram, `gaze`, `control`,
`move`), so the rules are the session's. The peer's `ft-together` and the panel's owner say
whether it may, and the command gives their answer: `act` on a panel it wasn't given is refused,
and so is `put` unless the peer owns the room or was given the panel. A negative number needs
`--` first (`look -- -20 5`, `act 0 scroll -- 0 -120`).

`point` and `act` are two things, as they are for a real hand: `point` is what the room sees,
`act` is what the owner's desktop gets. A real guest's hand does both with each move.

**Control is the owner's to give.** `ask` only asks. For a panel of this Frame's, its wearer
gives it on the Frametop Together page (or `together/ft-room control give NUMBER PERSON`), and
only while they wear the headset. The testbench has no command and no tool that gives a panel
of yours: `give` gives the peer's own ("Its own panel", below).
Once a panel of yours is given to a fake peer, its `act` commands move your pointer and type on
your desktop until you take it back, it leaves, or you take the headset off.

When the peer leaves, goes away or joins a room, its pointer comes off the panel first and it
stops sharing where it looks. `state` says what it points at (`points`), where it looks
(`looks`), and what its `ft-together` says of control (`control`: the panels it asked for and
holds) and of its gaze (`gaze`).

## Its own panel

`share` puts one panel of the peer's own into the room: a test picture that counts, two metres
wide and flat, named "Test panel". It is the peer's panel number 0, so its id in the room is the
peer's place times eight.

| Command | What the peer does |
| --- | --- |
| `share [--size WxH] [--at X Y Z] [--turn DEGREES]` | Shares the panel, or moves the one it shares. The picture is 1280x720 unless `--size` says: W one of 640, 896, 1280, 1408, 1664 or 1920, H a multiple of 8 from 360 to 1200. `--at` is its middle in the room in metres, and `--turn` a turn to its left; without them it stays where it is, or stands on a spot of the peer's own. Another size takes the panel back and shares a new one |
| `unshare` | Takes the panel back |
| `give PERSON` | Gives its panel to the person in that place of the room (0 is the room's owner), asked or not |
| `take [PERSON]` | Takes it back from them, or from everyone, and says no to their ask |

The picture is made by `ft-sharepanel --pattern`, the program a Frame shares a screen with, fed
a test picture in place of a screen. It runs this Frame's hardware encoder, and only while
someone watches the panel; the encoder closes three seconds after the last watcher has gone.
When this Frame's wearer watches it, the Frame encodes and decodes the same video. The widths
are the ones the Frame's encoder is known to take: at some others it resets the Frame
([together/README.md](../../together/README.md), "What the Frame's encoder needs").

While it shares, the peer does what a Frame's own programs do for a shared screen. It says each
second that its wearer is there (`control here`), which is what lets it give the panel and keeps
a grant standing; a peer that is away doesn't say it, and what it gave ends, as it does for a
real wearer. It takes where the others put its panel (`moves`) and tells the room that it stands
there now. And it takes the input of whoever it gave the panel to (`inputs`). There is no
desktop behind the panel, so the input goes nowhere: `state` counts it (`shares.inputs`) and
lists the last sixteen acts (`shares.last`: who, what, and its two numbers), which is how a test
sees that your hand reached it. `state` also says the panel's size, where it stands and whether
its picture's program runs.

The peer takes its panel out of the room before it leaves or joins another. While it is away the
panel stays and its picture keeps counting, where a real Frame's would freeze.

## Its PC

`pc` gives the peer a split compute PC, and puts that PC's windows into the room as panels of
the peer's: up to four, numbers 3 to 6 of its panels, so their ids are the peer's place times
eight plus 3 to 6. Each is 640x360 and a metre wide, named "Window 1" to "Window 4", in a row
above the peer's own panel. With its own panel a peer then has five in the room, and three
peers fifteen: more than a Frame decodes at once, which is what the decoder budget is for
([together/README.md](../../together/README.md), "A shared panel's video").

| Command | What the peer does |
| --- | --- |
| `pc [WINDOWS]` | Its PC comes, and that many of its windows (1 to 4) are in the room. Without a number: as many as it had, or one. Said again, it changes how many |
| `unpc` | The windows leave the room, and the PC goes |

The PC is `ft-pcsend`, the program that stands for a PC's sender
([docs/together.md](../../docs/together.md), "A PC's panels"). It calls the peer's own
`ft-together` with a one-time code that one made for it, as a real PC does, hears what each
window is to be encoded at, and sends pictures for the windows someone watches. The peer's
`ft-together` passes them on to the watchers. The pictures are a loop that was encoded once
(`together/assets/pcloop-640x360.bin`: a second of the test picture that counts), played again
and again from its keyframe, so no encoder runs for a PC's windows, however many there are,
and what a watcher decodes counts up thirty pictures and begins again. The loop plays slower
where an encoder would encode less often, and is never encoded at another bitrate.

A PC's windows stay where `pc` put them: where someone else puts one isn't taken. Nothing is
given on them, since a PC's panels take no input yet. The peer takes them out of the room
before it leaves or joins another, and its PC stays with it. `state` says under `pc` whether
the PC's program runs and how many windows are in the room.

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
no path in it, a panel's id is 0 to 31, every number of a pointer, a look, an act or a place
is within what the session takes, and its own panel's size is one the encoder is known to take),
and reads nothing a room sends: the programs it starts do, by the session's own
rules. `stop` asks it to end over its socket, since a signal to the unit doesn't reach a program
in the container; it then leaves its room and ends its programs. One that doesn't end in 15
seconds is told with SIGTERM, and the command says so.

A fake peer listens where a real guest's Frame would: on the tailnet, behind the same rules as
the Frame's own `ft-together` (a caller needs an open room there and a code, and a fake peer
never opens a room). `start --loopback` keeps its links on 127.0.0.1, for a room whose owner
also runs with `--loopback`; the tests use it.

What is real: the session, the links (QUIC between the two `ft-together` on this Frame), the
room's rules, the pose datagrams, the Opus voice and its mouth shapes, an avatar's file crossing
a link, the pointers, the asks and grants, the input a grant lets through, its panel's
video, encoded by the Frame's encoder and sent as a screen's is, and its PC's call, code and
pictures on their way through the peer's session. What isn't: the
wearer (a pose that plays, not a tracked head and hands), the voice (a hummed note, with no
microphone opened and nothing played from the peer's end), the hand and the eyes (a place and a
direction it is told, not a pointer on a panel it sees or a gaze that was tracked; the body's
pose doesn't follow either), its panel (a test picture with no desktop behind it), its PC (a program on this Frame that plays a
loop, with no window, capture or encoder), and the
network (both ends are on this Frame, so there is no delay
or loss). While a peer is away nobody stands
there: its pretended wearer is ended until it is back, as `ft-avatars --room` tells the room
nothing of a wearer who is away.

## Models

`wear NAME` has the peer wear a model of this Frame's avatars folder
(`~/.local/share/frametop/avatars`, or the folder `start --models` names). The others get its
file only if it is marked as one to pass on, as for a real wearer
([avatar/README.md](../../avatar/README.md)); otherwise they see the abstract figure, and
`state` says why in `wear`.

## What it can't do yet

- Share more than one panel of its own, a floating window, or anything with a desktop behind
  it. What is done on its panel is counted, not shown in its picture.
- Have its PC's windows moved by someone else, or give them: a PC's panels take no input yet.
- See a panel: it runs no `ft-peerpanel`, so it points where it is told, not where a picture is.
- Speak from a recording. It hums.
- A slow or lossy link: no delay, loss or rate cap between it and the Frame.
- Run on another computer and call over Tailscale.

## MCP tools

`peer_start`, `peer_stop`, `peer_state`, `peer_join`, `peer_leave`, `peer_pose`, `peer_walk`,
`peer_avatar`, `peer_speak`, `peer_away`, `peer_point`, `peer_look`, `peer_control_ask`,
`peer_act_move`, `peer_act_button`, `peer_act_scroll`, `peer_act_key`, `peer_put`, `peer_share`,
`peer_unshare`, `peer_control_give`, `peer_control_take`, `peer_pc` and `peer_unpc` ([mcp.md](mcp.md)). The tools join
only the room open on this Frame and take no host, port, code, socket or folder. A peer gives
only its own test panel: none gives a panel of the wearer's.

## Tests

`tests/test_peersim.py` checks the simulator's rules with stand-in programs and, where
Frametop Together is built, runs a real `ft-peer-sim` into the room of an
`ft-together --loopback`: it is let in, stands, walks a metre, hums with its mouth moving, goes
away and leaves, and no program is left behind. `tests/test_peeracts.py` checks what each of
the hand's and the eyes' questions says to the session, with a stand-in for it, and in such a
room whose owner shares a panel: the owner's programs are told where the peer points, its look
is in its poses, an act is refused until the owner gave the panel, then reaches the program at
the owner's desktop in order, a `put` reaches the owner's Frame, and nothing does once the
panel is taken back. `tests/test_peershare.py` checks the questions about its own panel the
same way, and in such a room: the panel is in the owner's room and its picture's program waits
for a watcher, the owner's acts are refused until the peer gave it and then reach the peer, the
owner puts it elsewhere and the room hears that it stands there, a grant ends when the peer
goes away, and the panel goes when it is taken back or the peer leaves. Nobody watches the
panel in that run, so no video is encoded. A second run, only where the Frame's video devices
are, has the owner's `ft-peerpanel` watch it: sixty pictures decode at the size they were sent,
each saying which one it is and in order, the encoder opens when the watcher comes and closes
after it has gone (`tests/peerkit.py` has what the real runs share). `tests/test_peerpc.py`
checks the questions about its PC the same way, and in such a room: the PC calls the peer's
session and is let in, its windows are the peer's panels 3 to 5 in the owner's room, fewer and
none when told, and they leave with the peer while the PC stays. A second run, where the
Frame's video decoder is, has the owner's `ft-peerpanel` watch the fourth window: seventy
pictures decode, the loop's thirty round after round, and no encoder was opened.
`tests/test_peerline.py` checks the peer's line to its session against a stand-in. `tests/test_peers.py` checks the client and the
`peer` commands against stand-ins on the sockets, and once for real: a peer started, let into a
room, told to wave and stopped. `tests/test_mcp_peers.py` checks each tool's command and what
the tools refuse (it needs the MCP SDK, like the other MCP tests). Seeing and hearing a fake
peer in the headset is a headset test.
