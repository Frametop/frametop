# The gestures file (FHGEST01)

`/run/user/UID/frametop-hands/gestures`: pinches and grips, for input. ft-hands writes it (`track/pinch.cpp`); the pointer helper (`../pointer/helper/handsin.h`) and `tools/watch_gestures.py` read it. The C layout is `include/fh_gestures.h`, which also says what each gesture is. This page is the contract for other readers and writers.

The folder, the sequence lock and the rules for taking over the file from ft-hands are the hands file's: `include/fh_hands.md`, "The sequence lock" and "Producers". A producer that takes over the hands file and writes gestures too carries the counters on (below).

## Layout

Little-endian, natural alignment. Offsets in bytes.

| Offset | Size | Field | |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `FHGEST01` |
| 8 | 4 | `version` | 4 |
| 12 | 4 | `size` | 512 for version 4 |
| 16 | 8 | `seq` | the sequence lock |
| 24 | 8 | `capture_ns` | `CLOCK_MONOTONIC` time the cameras took the frames |
| 32 | 8 | `publish_ns` | `CLOCK_MONOTONIC` time this was written |
| 40 | 4 | `begin_m` | the pinch thresholds in use, m |
| 44 | 4 | `end_m` | |
| 48 | 4 | `grip_begin` | the grip thresholds in use, curl ratios |
| 52 | 4 | `grip_end` | |
| 56 | 8 | reserved | zero |
| 64 | 2 × 64 | `pinch[2]` | thumb to index; [0] left hand, [1] right |
| 192 | 2 × 64 | `grip[2]` | version 2: a closed hand |
| 320 | 2 × 64 | `pinch2[2]` | version 3: thumb to middle fingertip |
| 448 | 16 | `producer` | version 4: the writing program's name, NUL-padded: `ft-hands` |
| 464 | 48 | reserved | zero |

Each slot (`fh_pinch_t`, 64 bytes):

| Offset | Size | Field | |
| --- | --- | --- | --- |
| 0 | 4 | `flags` | `FH_PINCH_TRACKED` 1, `FH_PINCH_DOWN` 2, `FH_PINCH_LOST` 4 |
| 4 | 4 | `hand_id` | the hands file's `id` of the hand, 0 if none |
| 8 | 4 | `begins` | gestures begun so far |
| 12 | 4 | `ends` | gestures ended so far |
| 16 | 8 | `begin_ns` | capture time the current or last one began |
| 24 | 8 | `end_ns` | capture time the last one ended |
| 32 | 4 | `distance` | pinch: tip to tip, m; grip: the fingers' mean curl |
| 36 | 4 | `strength` | 0 open .. 1 closed |
| 40 | 12 | `point[3]` | where the gesture is now |
| 52 | 12 | `begin_point[3]` | where it began |

## Units and frames

As the hands file: metres, `CLOCK_MONOTONIC` nanoseconds, positions in the head frame at the capture time (OpenVR's HMD frame, +x right, +y up, -z forward). `point` is at `capture_ns`, `begin_point` at `begin_ns`: turn each into the room with the head's pose at its own time before subtracting them for a drag, so turning the head doesn't drag.

## Counting

`begins` and `ends` count every gesture, so a reader that polls slower than a quick tap still sees it. `begins - ends` is 1 while one is down. They never go back: a writer that opens an existing file with the right magic keeps them, and one that finds a gesture left down (its writer died) ends it, flagged `FH_PINCH_LOST`. A producer taking over from ft-hands does the same.

## Versions

| Version | Size | Adds |
| --- | --- | --- |
| 1 | 192 | pinches |
| 2 | 320 | grips |
| 3 | 448 | middle pinches |
| 4 | 512 | the producer's name |

The file only grows at the end. A reader checks `magic`, accepts any `version` at or above the one it knows, copies the size it knows (`min(size, its own sizeof)`), and zeroes what the file is too short to hold: a version 3 file read with the version 4 header has no producer. It treats a group as absent unless the file's `size` reaches its end (the pointer helper drops the middle pinches from a file shorter than offset 448, where they end). Reserved bytes are ignored and written as zero.
