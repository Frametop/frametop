# G9 Frametop Together

The group's notes, from [the charts' README](README.md): what the chart and the model cover, their configs, and what's held by tests instead.


Covers the nine Together units and their switch, the session's modes on one Frame, a link at the owner's end and a guest's links to the owner and to the other guests, away, the voice and the mouth it moves, a screen shared with the room and someone's panel shown here, the avatar someone has on and the one you send. And what came with pointers, grants and moving: a panel of yours asked for, given and taken back, whose turn it is on your desktop, your hand on someone's panel and when it is kept quiet, where you look and when the room gets it, the others' marks on a panel, moving yourself through the room, and someone's panel in your hand. [docs/together.md](../together.md) has the decisions, and [together/README.md](../../together/README.md) the rules the chart's arrows name.

Model: `G9-together.tla` (module `G9Together`): who is in a room, by the owner's count and by each guest's, and the links between them. One owner and two guests. A link has two ends that each hear of its loss in their own time, and messages on a link keep their order. The wearers and the network may do anything at any time, within a config's counts. Configs: `Room` (the code: three calls to the owner, one link the network breaks), `Full` (a room with one place for a guest, four calls), `Reopen` (the owner opens a second room after the first), `Live` (a smaller room under fairness: left alone, it settles), `PreFix` (the code before `6e78c20`).

Not modelled: tailscaled's answers (everyone here is on the tailnet), versions, the limit on wrong codes and on strangers, away, panels, control and the turn, pointers, gaze, moving, avatars, voice, and time. A wait that runs out can do so whenever nothing is on its way on that link. The parts of the chart outside the session and its links are drawn from the code and its tests alone.

The rules: nobody is in a room the owner didn't let them into (InByAcceptOnly), a headset is in the room once (OnceInRoom), a place has one person (OneEachSeat), nobody has two links to one person (OneLinkEach), a guest holds a link to another only if the room lists them there (PeerIsListed), the owner's member links are the room's people (MemberIsListed), a guest's copy of the room is the owner's, now or earlier (CopyIsOwners), and once nothing is on its way everyone agrees who is in the room and each two of them have one whole link (Settled). Settles is the liveness rule: the room gets quiet again and again.

Results at `6e78c20`. The model and the session's rules for who is in a room have not changed since: the session now hands a member's messages it doesn't read to whoever does (control, moves), and closes the link of a member who says one they may not, which the model has as any word they may not say.

| Config | Rule | Result |
| --- | --- | --- |
| Room | All nine | Hold (719,774 states) |
| Full | All nine | Hold (1,126,402 states) |
| Reopen | All nine | Hold (266,158 states) |
| Live | Settles | Holds (14,237 states) |
| PreFix | OnceInRoom | Breaks, 10 steps (T1) |
| PreFix | The other eight | Hold (2,011,092 states) |

Fixed bugs:

| Id | Ticket | Rule | What happened | Fix |
| --- | --- | --- | --- | --- |
| T1 | DEE-365 | OnceInRoom | The owner hears that a link was lost up to ten seconds after the guest does. A guest who asked again in that time and was let in was listed twice, as was one whose two asks were both accepted: two figures for one person and a place nobody could have, until the old link's end was heard. Found by the model before anyone met it | Fixed in `6e78c20`. `Answer` closes the old link of a headset the room lists already and takes its old place out before it gives a place, and `Asked` doesn't count a headset's own listing against it. `together/tests/rejoin-test.cpp` runs both cases |

The model's shortest path to T1: `Open -> Invite -> Join -> GuestLeave -> Join -> DeliverO -> Invite -> Answer -> DeliverO -> Answer`. PreFix holds Settled all the same: the double listing went once the old link's end was heard.

The encoder and decoder path of a shared panel runs on the Frame's hardware since `bd03df2`, `acdef52` and `ce5299d` (the chart's green legend box). Given pictures at a width that isn't a multiple of 128, the Frame's encoder reset the Frame, twice on 2026-11-11; it also read past buffers that were only as long as the picture, and encoded each new buffer's first picture black. The encoder now refuses such a width before it opens the device, `tests/panels-test.py` holds all three on the hardware, and [together/README.md](../../together/README.md) ("What the Frame's encoder needs") has the rules. A reset on 2026-11-10 during the first run of that test was not pinned down.

A shared panel's video follows who sees it since `74c2dee`, `94e99d6`, `6e87517` and `67b480e` (the chart's parts "A screen of yours in the room" and "Someone's panel here"): its owner encodes it for the viewer who sees most of it and within the quality they picked, and a viewer's Frame decodes it only while its wearer sees it and the decoder has room, keeping the last picture otherwise. These rules are small sums with no room for a model (`together/flow/attention.h`, `budget.h`, `quality.h`), held by `together/tests/flow-test.cpp`, `video-test.cpp`, `budget-test.cpp` and `quality-test.cpp`, and on the hardware by `tests/panels-test.py`.

A split compute PC's panels are in the chart since `df78bee` (its part "Your split compute PC at this Frame"): the PC calls its person's Frame with a one-time code, is told what to encode, and its pictures go on to the room as that Frame's panels 3 to 6. The link is not one of the room's, so the model of the room and its links doesn't take it in; the rules are held by `together/tests/pcpanels-test.cpp` and, between the real programs, `together/tests/pcsend-test.py`.

The PC's own windows are in the chart since `5496ae9` (its part "The windows of your PC that you share"): `ft-roompanels` has the PC call in, tells it its panels and tells the room, and starts over when the PC says no call stands. A window is given only after a second ask, and a guest's input goes to it through that window's own stream (the arrow from the turn on your desktop into G6). The rules are held by `together/tests/test_roompclink.py`, `test_roompcs.py` and `roompcs-test.py`, `control-test.cpp`, and `screens/tests/peerinput-test.c` and `remote-test.c`; none of it is in the model.

Control is the part of this group where someone else reaches a desktop, and its rules are few: input is taken for a panel only from someone its owner gave it to, only while a program keeps saying the owner wears the headset, from one person at a time, and whatever they hold down is let go when any of that ends. They are checked in the owner's `ft-together` (`flow/control.cpp`, `room/grants.h`, `room/desk.h`), which the chart's legend says, and held by `together/tests/controlrules-test.cpp` (grants and the turn), `control-test.cpp` (several sessions against each other) and `roomcontrol-test.py` (the real programs) rather than by a model.
