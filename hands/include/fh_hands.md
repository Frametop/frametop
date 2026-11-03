# The hands file (FHHANDS1)

`/run/user/UID/frametop-hands/hands`: the hands a tracker found, for any program on the headset to read. ft-hands writes it (`track/io.cpp`); ft-screens' cutouts (`../screens/handcut.cpp`), ft-handview and the hand recorder (`rec/session.py`) read it. The C layout is `include/fh_hands.h`; this page is the contract, for a program that reads or writes the file without Frametop's source.

The folder is `/run/user/UID/frametop-hands/` with the user's numeric uid, not `$XDG_RUNTIME_DIR/frametop-hands`: a Frametop terminal has a private `$XDG_RUNTIME_DIR`, and the folder must be the same for every program. A writer creates it mode 0700.

## The sequence lock

The writer rewrites the file in place. It never truncates, replaces or unlinks it while it runs, so a reader can map it once.

Writer, for each publish:

1. store `seq` = an odd number (one more than the last even one), then a release fence;
2. write the fields;
3. store `seq` = the next even number, with release ordering.

Reader:

1. load `seq` (acquire); if it is odd, try again shortly;
2. copy the bytes it wants;
3. acquire fence, load `seq` again; use the copy only if it equals the first load.

`seq` changes with every publish. It starts again from 0 when ft-hands restarts (it clears the file then), so a reader tells a new publish by `seq` differing from the last one it used, not by it growing. A producer that takes over the file ("Producers") carries `seq` on from the file's value, and ft-hands does the same when it takes the file back. `include/fh_shm.hpp` is a reader that does all this, maps read-only, and remaps when the file is replaced or grows.

## Layout

Little-endian, natural alignment, no padding beyond what is listed. Offsets in bytes.

| Offset | Size | Field | |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `FHHANDS1` |
| 8 | 4 | `version` | 3 (see "Versions") |
| 12 | 4 | `size` | bytes of the structure the writer wrote: 4832 for version 3 |
| 16 | 8 | `seq` | the sequence lock |
| 24 | 8 | `capture_ns` | `CLOCK_MONOTONIC` time the cameras took the frames these hands come from |
| 32 | 8 | `publish_ns` | `CLOCK_MONOTONIC` time this publish was written |
| 40 | 4 | `nhands` | hands in `hands[]`, 0 to 2 |
| 44 | 4 | `ncapsules` | capsules in `capsules[]`, 0 to 64 |
| 48 | 16 | reserved | zero |
| 64 | 2 × 272 | `hands[2]` | `fh_hand_t` |
| 608 | 64 × 32 | `capsules[64]` | `fh_capsule_t`: segment ends `a`, `b`, radii `ra`, `rb` |
| 2656 | 2 × 1024 | `fits[2]` | version 2: `fh_hand_fit_t`, the skeleton of `hands[k]` |
| 4704 | 128 | `src` | version 3: `fh_hands_src_t`, the producer and the head pose |

A hand (`fh_hand_t`, 272 bytes): `id` (stays the same while the hand is tracked), `flags` (`FH_HAND_RIGHT` 1, else left; `FH_HAND_STEREO` 2, triangulated from two or more cameras), `confidence`, 4 reserved bytes, `pts[21][3]`, and `ncapsules`, this hand's share of `capsules[]` (they follow the previous hands').

A skeleton (`fh_hand_fit_t`, 1024 bytes) is described field by field in `include/fh_hands.h`: 21 joints, each a position, a rough radius and an orientation (x, y, z, w); 20 bone lengths; 26 joint angles; the elbow; the pinch point and contact; the palm's velocity and angular velocity in the head frame. `flags` has `FH_FIT_VALID` (1) when it holds one. `fits[k]` is zero when k ≥ `nhands`.

The producer and head pose (`fh_hands_src_t`, 128 bytes):

| Offset in `src` | Size | Field | |
| --- | --- | --- | --- |
| 0 | 16 | `producer` | the writing program's name, NUL-padded, at most 15 characters: `ft-hands` |
| 16 | 4 | `head_flags` | `FH_HEAD_VALID` (1): the next three fields hold a pose |
| 20 | 4 | `head_space` | OpenVR's `ETrackingUniverseOrigin`: 1 standing, 2 raw; 0 none |
| 24 | 8 | `head_ns` | `CLOCK_MONOTONIC` time of that pose: `capture_ns`, or the nearest the producer had |
| 32 | 12 | `head_p[3]` | m |
| 44 | 16 | `head_q[4]` | x, y, z, w |
| 60 | 68 | reserved | zero |

## Units and frames

- Lengths are metres, angles radians, times `CLOCK_MONOTONIC` nanoseconds.
- Every position and orientation is in the **head frame at capture time**: OpenVR's HMD frame, +x right, +y up, -z forward, at the moment the cameras took the frames (`capture_ns`), not when the reader looks.
- To place a hand in the room, take the head's pose at `capture_ns`. With `FH_HEAD_VALID` set, the file carries it: a head-frame point x is at `rotate(head_q, x) + head_p` in `head_space`. Without it (ft-hands has no pose, so it leaves the flag clear), the reader looks the head up itself, as `../screens/handcut.cpp` does from OpenVR's pose history.
- `pts` are MediaPipe's 21 hand landmarks, in its order: 0 wrist; 1-4 thumb CMC, MCP, IP, tip; 5-8 index MCP, PIP, DIP, tip; 9-12 middle; 13-16 ring; 17-20 little.
- The skeleton's joints are indexed the same way. Their orientations follow OpenXR's hand-joint convention: -Z along the bone that starts at the joint (toward the fingertip), +Y out of the back of the hand, +X = Y × Z. The wrist has the palm's frame. OpenXR's 26 joints add a palm and four metacarpal bases; a reader that needs them derives them (the palm as the middle knuckle's midpoint with the wrist, the bases on the wrist-to-knuckle line).

## Versions

| Version | Size | Adds |
| --- | --- | --- |
| 1 | 2656 | hands, landmarks, capsules |
| 2 | 4704 | a skeleton per hand, after version 1 |
| 3 | 4832 | the producer and the head pose, after version 2 |

The file only grows at the end; a field, once given a meaning, keeps it and its offset. So:

- A reader checks `magic`, then accepts any `version` at or above the one it knows, and reads the part it knows. To read version *n*'s fields it needs `version >= n` and `size` at least that version's size; otherwise it treats them as absent.
- A reader ignores reserved bytes and anything past the size it knows. A writer zeroes them.
- A reader must not require `size` to equal its own `sizeof`: a newer writer's file is larger.
- A writer writes the full current structure and stamps `magic`, `version` and `size` with every publish.

## Producers

ft-hands is the producer Frametop runs, but the file is not tied to it. Another tracker (another model, a tracker on a PC fed from the ring, a replay) can write the same file and every reader keeps working. It says who it is in `src.producer`.

Only one program writes at a time. To take over from ft-hands:

1. touch `/run/user/UID/frametop-hands/held` at least once a second for as long as it publishes, with its name as the file's content (for people reading the folder). ft-hands parks while the file is less than 3 s old: it stops tracking, writes no hands, and ends any gesture down, as lost. A producer that dies lets go by itself 3 s later. The one exception is a recording (`ft-hands --record`, or SIGUSR1 for a take): it never parks while it records, for `held` or anything else, so the take keeps every frame. It parks for `held` once the recording ends, after `--record-for` seconds at most (120 by default).
2. wait about 0.6 s after the first touch, so ft-hands' last publish is over (it looks every 0.25 s, then finishes the frame it is on). Then check that `seq` has stopped moving. If it still moves, ft-hands is recording and still publishes: keep touching `held` and wait until it stops.
3. open the existing file and write in place: map it, carry `seq` on from its value, and write the full version 3 structure. Never unlink, replace or truncate it; readers keep their mapping.
4. on exit, stop touching `held` (removing it is fine). ft-hands tracks again within a few frames of the file going stale and restamps the header as its own.

The gestures file (`include/fh_gestures.md`) goes with it: a producer that writes hands but not gestures leaves the gestures file as ft-hands left it, with nothing down. While ft-hands is parked for `held`, ft-camd keeps filling the ring (`camd/fhring.md`), so the other producer can track from it.

`park` (in the same folder) is the other way to park ft-hands; it is the input relay's, for VR games, and also stops the ring's mono frames. The testbench (`../testbench/`) parks it through its own marker.
