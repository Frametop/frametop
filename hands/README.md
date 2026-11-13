# Hand tracking pipeline

`fh-camd` (`camd/`, derived from Frametop's `ft-camd`) attaches to Steam Frame XRService's existing tracking cameras. It uses the V4L2 dequeue tracepoint and `pidfd_getfd` to read their DMA-BUFs without taking ownership of the cameras. The broker's file capabilities are needed only during setup and are dropped before the frame-copy loop. Its private ring is `/run/user/$UID/framehands/cam-ring` (`camd/fhring.h`).

`fh-hands` (`track/`, derived from Frametop's `ft-hands`) reads the ring, calibrated camera geometry from `/persist/xrservice.json` and `/persist/device_config.json`, and pinned MediaPipe/ncnn palm and landmark models. It detects both hands, identifies sides, triangulates 3D landmarks, fits a 21-joint skeleton, filters movement, and recognizes index pinches, middle pinches, and grips. The tracker publishes `hands` and `gestures` in the same private runtime directory. Their layouts are `include/fh_hands.h` and `include/fh_gestures.h`; consumers should use the sequence-lock pattern in `include/fh_shm.hpp`.

`models/ncnn/` contains the stock fallback. `models.sh fetch` downloads the fine-tuned model at the exact commit and hashes in `models.pin` into `models/student/`. It is licensed separately and ships with its own license and notice. `HANDS_MODEL=stock` selects stock; `latest` selects the fine-tuned model when present. The release packages both.

Build and offline tests are in `Makefile`; run them through `bash scripts/build.sh` from the repository root on ARM64 Linux. The source retains offline recording structures used by the tracker and its tests, but no recorder app or upload system is installed.
