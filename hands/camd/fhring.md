# The camera ring (FHRING01)

`/run/user/UID/frametop-hands/cam-ring`: the headset cameras' latest frames, which ft-camd copies out of XRService's buffers (`camd/camd.c`). ft-hands tracks from it; the hand recorder (`rec/session.py`), `tools/ring.py` and `camcheck.py` read it too. The C layout is `camd/fhring.h` and `../recorder/fhformat.py` mirrors it for Python (`../recorder/tests/test_fhformat.py` checks the two agree). This page is the contract for other readers.

What the pixels mean (which camera is which, its intrinsics and where it sits on the head) is not in the ring. It is in the calibration file next to it, `include/fh_calib.md`.

## The file

ft-camd creates the file when it starts: it unlinks the old one and creates a new one, mode 0600, sized once. A reader therefore maps it once per ft-camd, and opens it again when `writer_pid` changes, the file's inode changes, or the heartbeat stops.

- `heartbeat_ns` (`CLOCK_MONOTONIC`) is refreshed at least every 0.2 s while ft-camd runs. It is set to 0 when ft-camd exits. A reader treats a heartbeat older than about a second as a writer that is gone.
- `file_bytes` is the size the writer made. A reader refuses a file shorter than that.

## Layout

Little-endian, natural alignment. Offsets in bytes.

The header (`fh_ring_hdr_t`, 1344 bytes):

| Offset | Size | Field | |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `FHRING01` |
| 8 | 4 | `version` | 1 |
| 12 | 4 | `header_bytes` | 1344 |
| 16 | 4 | `ncams` | cameras in `cams[]`, at most 8 |
| 20 | 4 | reserved | zero |
| 24 | 8 | `file_bytes` | the file's size |
| 32 | 8 | `writer_pid` | ft-camd's pid |
| 40 | 8 | `heartbeat_ns` | see above |
| 48 | 16 | reserved | zero |
| 64 | 8 × 160 | `cams[8]` | one per camera |

A camera (`fh_ring_cam_t`, 160 bytes):

| Offset | Size | Field | |
| --- | --- | --- | --- |
| 0 | 32 | `sensor` | the media entity, e.g. `og01a1b 4-0060` |
| 32 | 32 | `name` | a slug of the sensor, plus `-dark` for a dark-frame camera; not the calibration's name |
| 64 | 4 | `node` | N of `/dev/videoN` |
| 68 | 4 | `format` | 0 = 8-bit grey |
| 72 | 4 | `width` | pixels |
| 76 | 4 | `height` | |
| 80 | 4 | `stride` | bytes per row in the ring (today `width`) |
| 84 | 4 | `nslots` | slots for this camera (4) |
| 88 | 8 | `slot_offset` | file offset of slot 0 |
| 96 | 8 | `slot_bytes` | a slot's header plus its image, rounded up to 64 |
| 104 | 8 | `latest` | the newest published frame number, 0 for none yet |
| 112 | 8 | `published` | frames published |
| 120 | 8 | `dropped` | dark, stale or torn frames not published |
| 128 | 4 | `flags` | `FH_CAM_DARK` 1: the near-black exposures between a camera's frames; `FH_CAM_COLOR` 2: an Arcturus colour camera's luma, downscaled |
| 132 | 4 | `dark_mean` | mono: the mean luma of its latest near-black frame, 0 before the first |
| 136 | 24 | reserved | zero |

Slot s of camera c starts at `slot_offset + s * slot_bytes` and holds a 64-byte header (`fh_ring_slot_t`), then `height` rows of `stride` bytes:

| Offset | Size | Field | |
| --- | --- | --- | --- |
| 0 | 8 | `seq` | the slot's sequence lock (below) |
| 8 | 8 | `frame` | the frame number n |
| 16 | 8 | `capture_ns` | the V4L2 timestamp (below) |
| 24 | 8 | `dqbuf_ns` | `CLOCK_MONOTONIC` when XRService dequeued the buffer |
| 32 | 8 | `publish_ns` | `CLOCK_MONOTONIC` when the copy finished |
| 40 | 4 | `v4l2_seq` | the V4L2 sequence number |
| 44 | 4 | `mean` | mean luma on a sparse grid |
| 48 | 16 | reserved | zero |

## The sequence lock, per slot

Frame n of a camera goes in slot `n % nslots`. The writer stores the slot's `seq` = 2n + 1, writes the header fields and pixels, stores `seq` = 2n + 2 (release), then `latest` = n.

A reader takes `n = latest` (acquire), checks that the slot's `seq` is 2n + 2, copies what it wants, then reads `seq` again (after an acquire fence). If `seq` changed, the copy is torn: it starts over with the new `latest`. Holding a frame longer than `nslots - 1` frame times is not possible; copy it out.

Only complete, bright frames are published. A camera's frames are numbered from 1 within a run of ft-camd. A copy that turns out torn uses up its number and leaves its slot's `seq` at 0, so a gap in the numbers a reader saw is either frames it missed or frames that were dropped (`dropped` counts the latter).

## Clocks

- A mono tracking camera's `capture_ns` is its V4L2 timestamp, which on the Frame is `CLOCK_MONOTONIC_RAW`. ft-hands converts it to `CLOCK_MONOTONIC` by the two clocks' current offset (`raw_minus_mono_ns` in `track/io.cpp`) before it writes `capture_ns` in the hands file.
- A colour camera's `capture_ns` is on its own clock and isn't synced with the mono cameras. Line it up with them by `dqbuf_ns`.
- `dqbuf_ns` and `publish_ns` are `CLOCK_MONOTONIC` for every camera.

## Who gets frames

ft-camd copies the mono cameras' frames only while someone wants them. ft-hands writes `0` to `hands-want` in the same folder every second while it is parked for a game. A `hands-want` file newer than 3 s that says `0` stops the mono copies; no file, or a stale one, copies as usual. ft-hands parked for another producer (`held`, `include/fh_hands.md`) does not write it, so that producer keeps getting frames.

## Versions

Version 1. The readers that ship today (`track/io.cpp`, `tools/ring.py`, `rec/session.py`, `camcheck.py`) accept version 1 only. So a compatible addition goes in the reserved bytes and keeps the version at 1, with a zero meaning "absent". A version 2 is for a change those readers must refuse. A new reader should check `magic`, accept the versions it knows, use `header_bytes`, `slot_offset` and `slot_bytes` from the file rather than its own `sizeof`, and ignore reserved bytes.
