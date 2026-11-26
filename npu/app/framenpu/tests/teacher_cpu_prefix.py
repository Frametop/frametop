"""In-process ncnn prefix adapter; model loading is separate from frame timing."""

import ctypes
import hashlib
import json
import os


class CpuPrefix:
    def __init__(self, library, model, *, fp16):
        import numpy as np

        record = json.loads(library.with_name("prefix.json").read_text())
        source = library.parents[1] / "tools/ncnn_prefix_bridge.cpp"
        if hashlib.sha256(library.read_bytes()).hexdigest() != record["library_sha256"]:
            raise ValueError("CPU prefix library hash differs")
        if (
            source.exists()
            and hashlib.sha256(source.read_bytes()).hexdigest() != record["source_sha256"]
        ):
            raise ValueError("CPU prefix bridge source hash differs")
        lib = ctypes.CDLL(str(library))
        lib.fnpu_prefix_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        lib.fnpu_prefix_create.restype = ctypes.c_void_p
        lib.fnpu_prefix_destroy.argtypes = [ctypes.c_void_p]
        lib.fnpu_prefix_destroy.restype = None
        array = np.ctypeslib.ndpointer(dtype=np.float32, ndim=3, flags="C_CONTIGUOUS")
        lib.fnpu_prefix_run.argtypes = [ctypes.c_void_p, array, array, array]
        lib.fnpu_prefix_run.restype = ctypes.c_int
        self.lib, self.record = lib, record
        self.outputs = {
            "8": np.empty((64, 56, 56), np.float32),
            "10": np.empty((16, 56, 56), np.float32),
        }
        self.handle = lib.fnpu_prefix_create(
            os.fsencode(model / "hand.ncnn.param"), os.fsencode(model / "hand.ncnn.bin"), int(fp16)
        )
        if not self.handle:
            raise ValueError("CPU prefix model creation failed")

    def compute(self, rgb):
        import numpy as np

        if rgb.shape != (3, 224, 224) or rgb.dtype != np.float32 or not rgb.flags.c_contiguous:
            raise ValueError("CPU prefix requires contiguous FP32 RGB")
        if not self.handle:
            raise ValueError("CPU prefix is closed")
        status = self.lib.fnpu_prefix_run(self.handle, rgb, self.outputs["8"], self.outputs["10"])
        if status:
            raise ValueError(f"CPU prefix execution failed: {status}")
        return self.outputs

    def close(self):
        if self.handle:
            self.lib.fnpu_prefix_destroy(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()
