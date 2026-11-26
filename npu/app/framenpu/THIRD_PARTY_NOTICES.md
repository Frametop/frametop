# Provenance and third-party materials

FrameNPU's host library, SDK sources, examples, and build scripts are provided
under the repository's MIT license. The bundled runtime.bin and vector_add.fjob
are generated from those sources, not copied firmware images. The kernel
manifest records the payload identity and corresponding source hashes.

Valve/Qualcomm firmware, Qualcomm SDK libraries, and proprietary model weights
are not distributed. HMX sequencing is recovered at runtime from firmware
already installed on the supported device. That firmware remains subject to
its provider's terms; this license does not cover it.

The XOR model and small ncnn graph fixture are original synthetic examples.
LLVM, Python, libatomic and GitHub Actions are external tools/services with
their own licenses; they are not vendored.

The [convolution reference workflow](docs/CONVOLUTION.md) builds external ncnn
(BSD-3-Clause) and follows FrameTop's MIT-licensed crop transform. The v20a
weights are attributed to **Frametop (DeeJanuz)** and licensed CC BY-NC 4.0;
they are not redistributed or covered by this repository's MIT license.
The recorded source frames/crops and generated tensor dumps remain private.
Only source adapters, hashes, ROI metadata and sanitized numerical summaries
are included. Optional NumPy/Pillow preparation dependencies are external.

## Tracker integration patch

The [steam-frame-hand-tracking patch](integrations/steam-frame-hand-tracking.patch)
contains context from spyjacob's MIT-licensed tracker, copyright (c) 2026 spyjacob,
based on upstream commit `8852ec9bdd4f81fb5a359e5bf88ed25f0e6fc89e`.
Its [upstream license](integrations/steam-frame-hand-tracking.LICENSE) is included.
The tracker and its model weights are not bundled with the Python wheel.

## Primary research references

Referenced projects are not redistributed here.

- [Valve SteamOS issue 2890](https://github.com/ValveSoftware/SteamOS/issues/2890)
- [Published Hexagon bare-metal driver](https://github.com/evlaV/deckard-linux/blob/sm8650-7.2/drivers/soc/qcom/hexagon-baremetal.c)
- [Qualcomm-authored HMX encodings in QEMU](https://github.com/qemu/qemu/tree/master/target/hexagon/imported/hmx)
- [Qualcomm-authored HVX instruction semantics in QEMU](https://github.com/qemu/qemu/blob/master/target/hexagon/imported/mmvec/ext.idef)
- [Investigated HMX tile/packing reference](https://github.com/haozixu/htp-ops-lib/blob/85eb88edcafd35afff1a43606a4c47eec9a0ca0b/src/dsp/ops/mat_mul.c)
- [Linux Qualcomm PLL implementation](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/master/drivers/clk/qcom/clk-alpha-pll.c)
- [ncnn model format](https://github.com/Tencent/ncnn/blob/master/docs/developer-guide/param-and-model-file-structure.md)

This is an independent project, not endorsed by Valve or Qualcomm.
