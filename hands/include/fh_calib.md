# The calibration file (FHCALIB1)

`/run/user/UID/frametop-hands/calib`: the tracking cameras' calibration, for the images in ft-camd's ring (`camd/fhring.md`). With it, a program that reads the ring (a recorder, another hand tracker, a tracker on a PC fed from the ring) can turn pixels into rays in the head frame without the headset's factory files, and without working out which ring camera is which. The C layout is `include/fh_calib.h`; the writer is `track/calibfile.cpp`.

ft-hands writes it when it starts and whenever it exchanges the side cameras, because it is the one that matches ring cameras to the factory calibration: by XRService's log or capture pipe, then by checking which side each hand appears on (`track/sides.cpp`). It keeps the file while parked, so another producer (`include/fh_hands.md`, "Producers") can use it. A file is stale when its `ring_writer_pid` differs from the ring's `writer_pid`: ft-camd restarted since, and the cameras may be in another order.

The folder and the sequence lock are the hands file's (`include/fh_hands.md`). The file changes rarely, so a reader may copy it once and again when `seq` moves.

## Layout

Little-endian, natural alignment. Offsets in bytes.

| Offset | Size | Field | |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `FHCALIB1` |
| 8 | 4 | `version` | 1 |
| 12 | 4 | `size` | 2112 |
| 16 | 8 | `seq` | the sequence lock |
| 24 | 8 | `publish_ns` | `CLOCK_MONOTONIC` time this was written |
| 32 | 16 | `producer` | the writing program's name, NUL-padded: `ft-hands` |
| 48 | 8 | `ring_writer_pid` | the ring's `writer_pid` these cameras are for |
| 56 | 4 | `ncams` | cameras in `cams[]`, at most 8 |
| 60 | 4 | reserved | zero |
| 64 | 8 × 256 | `cams[8]` | one per calibrated camera |

A camera (`fh_calib_cam_t`, 256 bytes):

| Offset | Size | Field | |
| --- | --- | --- | --- |
| 0 | 32 | `name` | the calibration's name, NUL-padded: `slam_left`, `slam_right`, `upper_left`, `upper_right`, or `color_video<N>` for an Arcturus colour camera |
| 32 | 4 | `ring_cam` | index in the ring's `cams[]` whose images these are; -1 if none |
| 36 | 4 | `node` | that ring camera's `node`, to check the match |
| 40 | 4 | `model` | the distortion model: 1 = KB4 |
| 44 | 4 | `width` | the images this is for, pixels |
| 48 | 4 | `height` | |
| 52 | 4 | reserved | zero |
| 56 | 32 | `fx`, `fy`, `cx`, `cy` | doubles, pixels |
| 88 | 64 | `k[8]` | doubles, the distortion by model; unused ones zero |
| 152 | 72 | `R[3][3]` | doubles, row-major |
| 224 | 24 | `t[3]` | doubles, m |
| 248 | 8 | reserved | zero |

A reader checks `ring_cam`'s `node` and size against the ring's camera before using a calibration with its images.

## Frames and the model

- The head frame is the hands file's: OpenVR's HMD frame, +x right, +y up, -z forward, metres.
- A camera's frame has +z along the optical axis, +x to the right of the image and +y down it.
- `R`'s columns are the camera's axes in the head frame (`R[i][j]` is axis j's component i) and `t` is the camera's centre there: a point c in the camera frame is at `R c + t` in the head frame, and a head-frame point p is at `Rᵀ (p - t)` in the camera's.
- The intrinsics are for the ring's images as they are, a colour camera's downscaled luma included, with pixel centres at integer coordinates.
- KB4 (`model` 1) is Kannala-Brandt, OpenCV's fisheye model, with `k[0..3]` = k1..k4. A camera-frame point (x, y, z) is at θ = atan2(√(x² + y²), z) from the axis; θd = θ (1 + k1 θ² + k2 θ⁴ + k3 θ⁶ + k4 θ⁸); its pixel is u = fx · θd · x / r + cx, v = fy · θd · y / r + cy with r = √(x² + y²) (the principal point when r = 0).

`tests/calibfile_test.cpp` checks these conventions with a projection written from this page.

## Versions

Version 1. Later versions add fields in the reserved bytes or after `cams[]` and new `model` numbers. A reader accepts any `version` at or above the one it knows, reads the part it knows, ignores reserved bytes, and skips a camera whose `model` it doesn't know.
