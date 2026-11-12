# Frametop Together: shared rooms, avatars and hand models (design notes)

Status: in design and being built on the branch `dee-193-together` (2026-11-10). Nothing here ships yet. The decisions come from an interview with Daenon on 2026-11-10; the spikes' results are filled in as they are measured.

Frametop Together lets up to four people with a Steam Frame share one room. One person starts a room. The others join over Tailscale, see that person's screens where they are, walk among them, point at them and, when allowed, use them. Everyone can bring panels of their own. Each person shows as an avatar, and can import one from VRM, glTF or FBX files. The same model code lets you pick the hand model Frametop draws, or import your own.

The request began as "is there a collaborative mode for multiple Frame sharing a workspace?", for helping a friend get started: "literal (even though virtual) hand holding."

## Decisions (2026-11-10)

### The session

| Question | Decision |
|---|---|
| Who can join | Frames running Frametop. A browser guest and a flat PC viewer are later work |
| How many | Up to 4 people, as a full mesh: everyone can share panels with everyone |
| Where they are | Remote. Lining up two headsets in one physical room (a two-point controller touch) is later work |
| Network | Tailscale is required. No server of ours. A peer whose path runs through a DERP relay gets a warning and a lower quality |
| Transport | QUIC, with msquic: reliable streams for state, grants and files, a stream for each video frame, datagrams for poses and voice (spike T3) |
| Joining | The owner starts a room and gets an invite (the Frame's tailnet name and a one-time code). A guest opens it, and the owner accepts in the headset, seeing the guest's Tailscale login. A saved friends list skips the code next time |
| The join prompt | Says once that anyone in the room can see, hear and record what is shared. Nothing enforces or marks recording |
| Whose room | The owner's layout is the room. The others join at spots the owner offers, and their shared panels arrive as a group near them |
| The owner leaves | The room ends for everyone, and each person's panels go back to their own layout |
| Away | A person who starts a VR game or takes the headset off is away: their avatar dims, their shared panels stop and keep the last picture, and the control they granted is taken back. Voice stays on during a game and mutes with the headset off. They come back on their own |
| Versions | Peers on another protocol version are refused with a message to update |
| Name | Frametop Together. An optional part in the installer, which needs Tailscale |

### Panels

| Question | Decision |
|---|---|
| What can be shared | Screens, floating windows and remote displays. Nothing is shared until you share it, panel by panel, and a shared panel shows a border |
| How many | Up to 3 panels encoded on the Frame per person (fewer if the encoder spike says so), and up to 4 more from a split compute PC |
| Remote displays | Re-encoded on your Frame, so a guest never reaches your PC. A Mac's displays count toward the Frame's 3 |
| Split compute | A PC with the Frametop windows role sends its own windows and displays straight to the peers from the Vibepollo fork. It only sends: peers get no Moonlight access to it |
| Encoding | One encode per panel, the same for every viewer |
| Quality | Each panel's rate follows the slowest viewer that is watching it. The owner picks the cap: Low (about 1 Mbit/s per panel, 15 fps), Balanced (about 3, 30 fps) or High (about 8, 60 fps) |
| Loading and unloading | By attention: full rate when focused, a reduced rate in view, and out of view a panel stops receiving after about 3 s and keeps its last picture. When the decoder's budget runs out, your own remote displays and PC windows keep their place, and the peers' panels you look at least become stills first |
| Sound | Voice is built in and comes from each avatar's head. Your desktop's sound goes to the others only if you turn that on; the room's voices are kept out of it |
| Locked | Sharing stops while the Steam PIN lock is on |

### Control and moving

| Question | Decision |
|---|---|
| A guest's pointer | Always shown, in the guest's colour. Guests point by default |
| Control | Granted panel by panel. A panel that drives one of your PCs asks a second time. A grant covers clicking, typing and moving that panel |
| Two people on one panel | One at a time: the last to act holds it, and it frees after about 1.5 s without input. KWin has one seat, so the turn is the whole desktop's: two people can't work on two panels of one Frame at once either |
| Moving panels | A panel's owner and the room's owner can always move it. Others need a grant |
| The clipboard | Never crosses to another person |
| Moving yourself | Walk. Pull yourself through the room by grabbing the air with both grips, or a key and the mouse. Jump to a person or a panel from a list. No thumbstick movement |
| Pulling a panel you can't move | Flies you to a reading spot in front of it. The panel stays where it is, and nobody gets a private copy |
| Gaze | Not shared unless you turn it on. Then only a smoothed direction leaves the Frame, for the avatar's eyes and a faint mark on the panel you look at. Never eye images |

### Avatars and hands

| Question | Decision |
|---|---|
| Formats | VRM and glTF, and FBX from an avatar's source files. Never from VRChat's cache or servers |
| An FBX skeleton | Matched by the usual Unity humanoid bone names. One that can't be matched is refused, with the missing bones named |
| Size | Import reduces an avatar to about 20,000 triangles, merges its materials, caps its textures at 2K a side and 16 MB in all, and maps its shaders to a toon or a lit shader. A preview shows before and after |
| The body | Upper body from three points (the head and two hands). Legs are hidden or stand still |
| The face | The mouth moves with the voice, and the eyes blink. Hair and cloth stay rigid |
| What the others see | Your avatar, if its licence allows sending it (a VRM's licence fields) or you confirm you may share it (glTF, FBX). They keep it in memory for the session only. Otherwise they see an abstract head and hands in your colour |
| What you see of yourself | Your avatar's hands, and your body when you look down if the spike shows it holds still. If it slides when the head turns, the body view stays an experimental switch. A mirror panel shows the whole avatar |
| Hand models | Frametop Settings gets a picker for the bundled hand models, and you can import hands from a glTF, VRM or FBX file. An avatar's hands become yours |
| Real hands | With Room View on, your real hands still show through the panels as cutouts. "Always show model hands" draws the model over them too |

### How it is built

| Question | Decision |
|---|---|
| Order | M0 spikes, M1 hand models, M2 avatars, M3 the session core, M4 pointers, grants and moving, M5 the budget and the PC sender |
| Branch | One long-lived branch in one worktree, merged into `experimental` once, at the end |
| Testing | New testbench tools and a fake peer, `ft-peer-sim`, which is the real session code running without a headset ([testbench/docs/peers.md](../testbench/docs/peers.md)). Testers with Frames before a release |
| Tickets | DEE-193 is the parent. DEE-362 to DEE-369 are the milestones, the hand model request and the test tools |

## What exists already

- **Showing a streamed panel.** A remote display is a child process that decodes into a ring of three RGBA buffers and tells ft-screens "frame I" over a socket pair (`screens/remote.c`). The panel has every screen control and follows the attention rules. A peer's panel is the same kind of panel with another program behind it.
- **A panel's pixels.** Every screen and floating window reaches ft-screens as a DMA-BUF from KWin, and a remote display as one of ft-stream's ring buffers. ft-screens can copy any of them on the GPU.
- **The encoder.** `/dev/video-enc0` is Qualcomm's iris encoder: H.264 and HEVC from 128 to 8192 pixels a side, with RGBA (`AB24`), NV12 or UBWC as its input, a bitrate that can change while it runs, and a control that forces a keyframe. vr-recorder drives it with the raw V4L2 stateful API at about 3% of a core for 1080p30. Encoding on the CPU made the headset stutter, so it is never used.
- **The decoder.** 16 sessions and about one 8K60 stream's worth of macroblocks, shared with remote displays and Steam Link ([remote-displays.md](remote-displays.md)). The encoder draws on the same 16 sessions and the same load budget (spike T1).
- **Drawn hands.** ft-handview draws each tracked hand on a square overlay placed at the hand, one half per eye, from a skinned model ([hands/rendered-hands.md](../hands/rendered-hands.md)). An avatar is the same trick at a person's size.
- **Leases for input.** [host-input.md](host-input.md) hands a keyboard and mouse over with a lease the other side has to keep renewing. A control grant works the same way.

## Architecture

```
            your Frame                                         a peer's Frame
 ┌─────────────────────────────┐                        ┌──────────────────────────┐
 │ ft-screens                  │                        │ ft-screens               │
 │   share tap: a shared       │                        │   remote screen: a peer's│
 │   screen's own buffers,     │                        │   panel, with every      │
 │   as KWin draws them        │                        │   screen control         │
 └──────┬──────────────────────┘                        └──────────▲───────────────┘
        │ dmabufs, "frame ID"                                      │ ring of RGBA, "frame I"
 ┌──────▼──────────┐                                    ┌──────────┴───────┐
 │ ft-sharepanel   │ one per shared panel               │ ft-peerpanel     │ one per panel shown
 │ GPU scale, iris │                                    │ iris decoder     │
 │ encoder         │                                    │                  │
 └──────┬──────────┘                                    └──────────▲───────┘
        │ HEVC access units                                        │ HEVC access units
 ┌──────▼──────────────────────┐      QUIC over         ┌──────────┴───────────────┐
 │ ft-together                 │◄────  Tailscale  ─────►│ ft-together              │
 │ room state, peers, grants,  │  streams: state,       │                          │
 │ voice, the links            │  grants, avatar files, │                          │
 └──────┬───────────┬──────────┘  video frames          └──────────────────────────┘
        │ poses     │ voice       datagrams: poses, voice
 ┌──────▼──────┐ ┌──▼────────┐
 │ ft-avatars  │ │ PipeWire  │
 │ peers'      │ │ mic in,   │
 │ avatars,    │ │ one stream│
 │ your body   │ │ per peer  │
 └─────────────┘ └───────────┘
```

**ft-together is the session.** It owns the links to the peers, the room's state and its rules, and nothing else: no GPU, no V4L2, no SteamVR. That keeps it small enough to test without a headset, lets the fake peer be the same program, and lets the PC sender in the Vibepollo fork speak the same protocol without any of the Frame's code. It is MIT like the rest of Frametop.

**Media ends are separate programs.** `ft-sharepanel` encodes one shared panel and `ft-peerpanel` decodes one panel of a peer's. Each talks to ft-together over a local socket in encoded access units, and to ft-screens in DMA-BUFs. A stalled encoder or a decoder reset can't freeze the session or the screens, and each program has one job.

**ft-screens changes little.** It gains a share tap (`screens/share.c`): a shared screen's buffers, as KWin draws them, also go to that panel's `ft-sharepanel` as DMA-BUFs, one picture at a time and at most at the stream's rate, and only while someone watches. The GPU pass that scales a picture to the stream's size is in `ft-sharepanel`, next to the encoder it feeds, so ft-screens draws nothing for a share and a stalled encoder holds up nothing in it. It is the reverse of a remote screen. A peer's panel is a remote screen whose program is `ft-peerpanel` instead of ft-stream (`remote <N> peer`), so it has every control a screen has.

**ft-roompanels keeps the two in step with the room.** It is the one place that knows both sides: it asks ft-together what the room holds and ft-screens what stands on this Frame, makes a remote screen for each panel the others share and puts it where the room says, and tells the room where each screen you share stands. The sums between your own floor and the room are its own (`together/roompanels/placing.py`). Neither ft-together nor ft-screens learns of the other.

**ft-avatars draws people.** It loads avatars in Frametop's own model format, poses them from the poses ft-together hands it, and draws each on a stereo overlay placed at that person, as ft-handview does for a hand.

**Models have one format on disk.** `ft-model-import` reads VRM, glTF and FBX and writes Frametop's own file: a skinned mesh, a skeleton with its humanoid mapping, the mouth and blink shapes, and textures. Reducing happens there. ft-handview and ft-avatars read only that format. What a peer sends is that file too, so the code that reads a stranger's avatar is one small, strict loader, not three format parsers. The file, the importer and their limits are in [avatar/README.md](../avatar/README.md). Since M1 the file holds hands, and since M2 the body: more chunks, which the hands' reader skips.

### The protocol

One QUIC connection per pair of peers. A guest opens the one to the room's owner when they join; of two guests, the one whose Tailscale address is lower opens theirs. Each side checks the other's address with tailscaled before anything else. The messages, who may send each, and the steps of joining are in [together/README.md](../together/README.md).

- **A control stream** (reliable, ordered): hello with the protocol version, the invite's code, the room's state (people, their colours and spots, each shared panel's id, size, pose and curve), what a guest shares, what each viewer has of a panel, grants, a panel put elsewhere, away, and leave.
- **File streams** (reliable): an avatar's model file, when its owner allows.
- **A stream for each video frame** (one way): a panel id, a frame number, and the encoded picture. QUIC sends a lost packet again, so one lost packet costs that frame a round trip, not the picture. The sender gives a frame up (resets its stream) when it hasn't all been acknowledged 150 ms after it was sent, and the next frame it sends for that panel is a keyframe.
- **Datagrams**: poses (head and hands as the frames of the palm and finger bones, about 30 times a second), pointers (where on a shared panel someone points, while it moves) and voice (Opus, 20 ms). Each fits one datagram, at most 1187 bytes, and a late one is worth nothing.

Video was going to ride in datagrams too, as Moonlight's does. Spike T3 changed that: a frame is about eleven datagrams, so with one packet in a hundred lost, one frame in ten arrived broken and would have needed a keyframe. A stream for each frame delivered every frame on the same link. Moonlight gets there with error-correcting packets; QUIC's own retransmission needs no code of ours.

A viewer tells the sender how much it sees of each panel (focused, in view, hidden), and the sender stops sending a panel nobody watches. The links use BBR congestion control, with msquic's send buffering off so the sender knows when a frame was acknowledged. Nothing saves a sender that sends more than a link carries: frames arrive late and are given up. So each panel's bitrate follows the slowest link that is watching it, by how many frames had to be given up on it (msquic's own estimate of a link is not used so far).

### Trust

Everything a peer sends is untrusted input, including after the owner accepted them.

- A peer is who tailscaled says they are. The invite's code only proves they were invited; it is used once and expires.
- A panel is sent only if its owner shared it, and input is taken only for a panel with a grant that hasn't lapsed. Both are checked where they are enforced: in the sharer's ft-together, not in the viewer's.
- A panel is moved for the room only by its owner's Frame. Anyone else's word that they put it elsewhere is taken there from the room's owner, or from someone who holds that panel's grant now, and only for a place a panel can have. A viewer who may not move a panel moves only their own view of it, and that is put back.
- A grant is a lease, renewed while its owner stays present and taken back when they go away, lock the Frame or end it. Present means the pointer helper says the headset is worn: `ft-roomaway` tells `ft-together` so each second, and a grant ends ten seconds after the last word. (Frametop has no lock yet.)
- A grant names a panel, and the pointer goes only there. Keys go wherever the keyboard focus is on that desktop, so a grant is in effect leave to type on it while its owner watches. Keys that reach past the desktop (Meta, SysRq, power, sleep, volume) are never taken from a guest.
- Whatever a guest held down is let go by the owner's ft-together when their turn, their grant or their link ends. ft-screens lets go of it too, the moment the owner's own hand is on the desktop, and never releases for a guest what the guest didn't press.
- A guest's input reaches only the desktop's own screens, through `ft-roominput` and ft-screens' `peer` commands: never a remote display (another machine), Steam, or frunner's search.
- A peer's poses, sizes and counts are bounded before use. A peer's avatar file is read by the strict loader, with limits on every count and size, and never by the importers.
- Gaze, camera images and the clipboard never leave the Frame. Hand joints do.

## Milestones

| | What | Proved by |
|---|---|---|
| M0 | Spikes: the encoder's limits, a DMA-BUF into the encoder, msquic on the Frame, heat | The numbers below |
| M1 | Hand models: models loaded from files at run time, the picker in Frametop Settings, import of your own hands. **Built** (2026-11-10): the model file, `ft-model-import` for hands, ft-handview reading your folder, the Appearance page | Offline tests, snapshots of each model, the picker driven on the headset (to do: `HEADSET-TESTS.md`) |
| M2 | Avatars: the importer and its limits, upper-body IK, the mouth and blinks, the mirror panel, the body spike. **Being built**: the body in the model file, `ft-model-import` for whole avatars, the pose solver, `ft-avatars`' pictures of a posed body, wearing an avatar in the Appearance page, the mirror that shows it to you, its face (the mouth from your voice, blinks) and the body spike's experimental switch are done (2026-11-10); what is left is the headset tests | Offline tests on sample avatars, snapshots, the mirror on the headset |
| M3 | The session core: joining, the room's state, a shared panel on a peer's Frame, voice, the abstract avatar, away. **Being built**: the wire format (`together/proto`), the room's rules (`together/room`), the session's (`together/session`), the links (`together/net`) and `ft-together` are done with their tests, and a second `ft-together` joined the live Frame's room over its tailnet address (2026-11-10). Poses and the abstract avatar are done too: `ft-avatars --room` tells the room where you are and draws the others ([avatar/README.md](../avatar/README.md), "People in a shared room"), and a pretended person on a second `ft-together` was drawn on the live Frame (2026-11-10, the headset off the head; seen in the headset is still a headset test). Panels are under way: the room lists what each person shares (`share` and `unshare` on ft-together's socket), each picture crosses a link on a stream of its own and is given up when late, and ft-together carries a panel's video between the program that encodes it and the programs that show it by the rules in [together/README.md](../together/README.md), "A shared panel's video" (tested with stand-in programs over real links). The programs at each end are written: `ft-sharepanel` (the GPU scale and the hardware encoder), `ft-peerpanel` (the hardware decoder into a remote screen), the share tap and `remote <N> peer` in ft-screens, and `ft-roompanels`, which places the others' panels here and yours in the room ([together/README.md](../together/README.md), "A shared panel on a Frame"). The tap, the peer screens and ft-roompanels are tested without video, ft-roompanels between two real `ft-together` with a stand-in ft-screens. The video through the hardware runs: `together/tests/panels-test.py` sends a test picture from `ft-sharepanel` through the Frame's encoder, two `ft-together` and `ft-peerpanel`'s decoder at six sizes, and every picture comes back whole, in order and in its colours (2026-11-11, run by hand; what it took is in [together/README.md](../together/README.md), "What the Frame's encoder needs"). A real screen through ft-screens' tap and the GPU pass is still a headset test. Voice is done without sound devices: `ft-roomvoice` sends the wearer's voice as Opus datagrams and plays the others', each from where their head is, and two of them between two real `ft-together` heard each other's test note from the right side ([together/README.md](../together/README.md), "Voices on a Frame"); a real microphone and loudspeakers are a headset test. Away is done: `ft-roomaway` tells the room when the headset comes off or a game starts, from what the pointer helper and the relay say, and `ft-avatars --room` neither draws nor sends while the wearer is away. So is running it: five user units that start and stop together, an optional part of `install.sh` (off until turned on), and the Frametop Together page in Frametop Settings, where a room is opened or joined, people are let in, and the voice and the screens are shared ([together/README.md](../together/README.md), "Running it"); the page is tested against stand-ins for the programs. Avatars are sent between `ft-together`s: the room lists the model file each person has on by its hash, whoever lacks it asks its wearer, and it crosses on a stream of its own and is kept for the session only ([together/README.md](../together/README.md), "The avatars people have on"; tested between real `ft-together`s). `ft-avatars --room` says which avatar the wearer has on and draws the others in theirs: a sent file is read by the strict readers only, and one that isn't a whole avatar marked as passed on by its owner is refused ([avatar/README.md](../avatar/README.md), "People in a shared room"; tested with files and a picture, seen in the headset is still a headset test). The Together page asks before a model that can't say whether it may be passed on is first sent, and marks its file after a yes ([together/README.md](../together/README.md), "Running it"). Mouths move with the voices: `ft-roomvoice` makes the five mouth weights of each 20 ms it hears, ft-together puts them into the wearer's poses, and `ft-avatars --room` sets them on a sent avatar and blinks its eyes (tested between two real `ft-together` with a hummed note, and in a picture; seen in the headset is still a headset test). The session and everything around it are charted as group G9 in [docs/charts](charts/README.md), with a TLA+ model of who is in a room and the links between them; the model found that a headset let in again within ten seconds of losing its link was listed twice, which is fixed (`together/tests/rejoin-test.cpp`). The testbench's fake peers are in: up to three, each the real programs with a pretended wearer and a hummed voice, started, let into this Frame's room and driven with `frame-testbench peer` and the MCP `peer_*` tools ([testbench/docs/peers.md](../testbench/docs/peers.md)). Two of them joined a room on the Frame through the testbench (2026-11-11, loopback links, the headset off the head): its owner's `ft-together` got each one's poses 30 a second, and the humming one's voice and moving mouth. Their panels wait for the video through the hardware, and seeing and hearing them in the headset is still a headset test | A fake peer joining the live Frame, and its panel measured |
| M4 | Pointers, grants, moving panels and yourself. **Being built**: asking for, giving and taking back control, the lease, one at a time, the input's bounds and what is let go, and pointers between programs, all through `ft-together` with their tests, are done (2026-11-11; [together/README.md](../together/README.md), "Control of a shared panel"). `ft-roominput` gives the input to the desktop through ft-screens' `peer` commands, with the turn-taking on the seat in `screens/peerinput.h` (2026-11-11; tested against stand-ins, not yet on a live desktop). The guest's side is `ft-peerpanel`: what ft-screens says of the wearer's pointer and keys on someone's panel becomes their pointer for the room and `control act` for its owner (`together/local/panelhand.h`, 2026-11-11; the rules and the sender tested between real `ft-together`s, the program itself not run with it). `ft-roompoints` draws the others' pointers on the panels they point at, each in its owner's colour, as SteamVR overlays put on the panel's surface (`together/points`, 2026-11-11; tested with `--probe` at one of three real `ft-together`s, and its drawing read back from SteamVR on the Frame against a stand-in panel, bent, flat, pinned and hidden; not yet seen in the headset). Moving yourself is built as commands (2026-11-11): your floor moves in the room (`floor move`) and everything on this Frame moves the other way (ft-screens' `shift`), so the room stands still; `go` to a place, a person or a panel and `pull` on `@ft_roompanels` do it ([together/README.md](../together/README.md), "Moving yourself"; the sums and the programs tested against stand-ins and between real `ft-together`s, ft-screens' own half compiled but not yet run on a desktop). Grabbing the air says `pull` for you (`ft-roommove`, 2026-11-11): with both grips down the room goes where the hands go, read from SteamVR's web socket and ft-screens' `reach` (tested with the grips told to it, between a real `ft-together` and `ft-roompanels`; not yet with a real grip in the headset). A key steps and turns you: `ft-room step` is on the PATH of the input relay's `command:` actions and of your own commands, with nothing bound by default (2026-11-11). Moving panels is built (2026-11-11; [together/README.md](../together/README.md), "Moving someone's panel"): a panel stays a screen of its owner's Frame, so someone else who puts it elsewhere tells its owner (a Move on the control stream), whose Frame takes its screen there and tells the room as it does for its own wearer. The room's owner may always, others with the panel's grant, checked in the owner's `ft-together` (`flow/moves.h`). A viewer who may not gets the panel back once it rests, and if they had pulled it more than 30 cm nearer, they are flown to where it reads well in 0.4 s (`fly panel` on `@ft_roompanels`). Tested as rules, and between real `ft-together`s and `ft-roompanels` with a stand-in ft-screens; not yet with a real panel in a real hand. Frametop Settings' Together page has the asks and grants and the list to jump from (2026-11-11; [together/README.md](../together/README.md), "The page"): who asks for which of your screens with Give it and No, what you gave with Take it back, every panel in the room with Ask to use it and Go to it, and Go to them beside each person (tested against stand-ins for the programs, not yet used in the headset). Holding a key and moving the mouse comes next, an ask shows only on that page, and the second ask for a panel that shows a PC waits until such panels can be shared | A fake peer asking for control and clicking |
| M5 | The budget, and the PC sender in the Vibepollo fork | Fake peers past the budget; a PC panel on a fake peer |

## Spikes

| | Question | Result |
|---|---|---|
| T1 | How many encoder sessions run at once, and at what load, next to SteamVR | **Passed.** 13 encoder sessions opened while three remote displays held decoder sessions: 16 in all, one pool for both. The load budget is shared too: three 4K30 or one 4K60 session opened next to those three displays, two 4K60 did not (`REQBUFS` fails with `ENOMEM`). Three sessions, the Frame's cap, run at any size up to 4K30. A picture takes 6 to 13 ms from queueing to its encoded unit. The bitrate and the keyframe controls work on a running session |
| T2 | Does the encoder take a GBM buffer as a DMA-BUF, with no copy | **Passed.** A linear `ABGR8888` GBM buffer has the encoder's own layout (stride 4 x width), and the encoder takes it with `V4L2_MEMORY_DMABUF`. Decoded on the CPU, the colours come back right as BT.709 full range; the stream itself says limited range and no matrix, so the receiving end converts as 709 full range and doesn't read the stream's flags. The first real runs (2026-11-11) added three rules, in [together/README.md](../together/README.md), "What the Frame's encoder needs": the picture's width is a multiple of 128, the buffers are as long as the encoder reads, and a new buffer is put out of the CPU's caches before its first picture |
| T3 | Does msquic build for the Frame's container, and what do its datagrams carry over Tailscale's 1280-byte MTU | **Passed, and it changed the design.** msquic 2.6.3 builds in seconds as a shared library on the container's OpenSSL 3.5 (the host's is older, so Together's programs run in the container like the rest). On a link with a 1280-byte MTU a datagram carries 1187 bytes from the first packet and 1219 once the path is probed. With 20 ms each way and 1% loss, three 3 Mbit panels as datagrams lost one frame in ten (CUBIC also queued them for seconds); as a stream for each frame with BBR, 723 of 723 frames arrived, in 28 ms on average and 184 ms at worst. Seven 2 Mbit panels: 1686 of 1687. Past what the link carries, frames are late whatever carries them |
| T4 | Heat and CPU with three 1080p panels encoding | **Passed on the bench.** Three 3440x1440 sessions at 30 fps for a minute each way: 9% of one core for a desktop-like picture, with the codec's thermal zone flat at 45 to 50 C. To do: the same during real use in the headset, measured passively |
| T5 | Your own body when looking down: does a head-following stereo overlay hold still | **Built, to be judged in the headset.** A head-following overlay was not built: it would slide by the draw's delay on every head turn. The body is drawn on a room-fixed level square under the eyes, each eye's view through it as a window, so SteamVR's own per-frame placing keeps it still when the head turns; only moving the head trails, by one draw (22 ms). It is the experimental switch under the mirror in Appearance ([avatar/README.md](../avatar/README.md), "Your own body"). Whether the trailing shows when walking, and whether panels drawing over the body bother, is the headset test |
| T6 | Two pointers taking turns on KWin's one seat: focus, held keys and drags | **Built, to be judged in the headset.** One seat is shared by rule, not by a second seat: the guest's pointer and held buttons and keys are kept apart in ft-screens (`screens/peerinput.h`), the owner's hand lets go of the guest's at once, and the owner's resting laser or gaze moves nothing while the guest is at the pointer. Whether KWin's cursor, hover and drags behave under those turns is the headset test |

## Open questions

- **Why the encoder resets the Frame, and the reset of 2026-11-10.** Given DMA-BUF pictures 1366 pixels across, the hardware encoder hard-resets the Frame in the second it starts, with nothing in the kernel's log (twice on 2026-11-11; [together/README.md](../together/README.md), "What the Frame's encoder needs"). Together never gives it such a width now. It is a fault in the kernel driver or the firmware that a program with no rights can reach, and it is not reported to Valve yet. The first reset, on 2026-11-10 about nine seconds into the first run of `together/tests/panels-test.py`, came while a 640x360 picture was encoded and decoded, with the full test suite and two remote displays running. That part of the test has passed every time since, but not again under that load, so whether it had the same cause is not known.
- How a second Frame's owner sets up Tailscale sharing with the least steps, and what the invite looks like when the two are on different tailnets.
- Whether a PC on the tailnet can be reached directly by every peer, or only by its own Frame.
- A model's "may be sent" mark is set at import, or later by `ft-model-import --allow-share` when its person says yes on the Together page. The ask shows in a room only, and only on that page: someone who never opens it is seen as the abstract figure. Whether the Appearance page should ask too, and whether a mark should be taken off without importing again, is open.
- Sound from a shared remote display: it already plays on the sharer's Frame, so the opt-in desktop sound would carry it.
