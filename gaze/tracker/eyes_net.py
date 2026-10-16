"""The eye net: one eye camera's frame in, that eye's raw gaze out (frame-eyes' train/gaze).

A model folder holds model.json and the ncnn files it names:
  {"name": "eyes-v0", "variant": "s100", "input": [100, 128], "gaze_scale": 30, "out": 6,
   "param": "model.ncnn.param", "bin": "model.ncnn.bin", "in": "in0", "outname": "out0", ...}
The net's output holds both cameras' heads (OUT numbers each, camera 0's first): gaze yaw and
pitch / gaze_scale, pupil x / 512 and y / 400, eye open (logit), pupil visible (logit).

The gaze is raw: each person's own offset is left to the calibration (eyes_model.Calibration,
with this gaze where the pupil centre would be). So a calibration belongs to one model: ft-eyes
keeps each model's in its own folder.

ncnn fp16 on one thread: 1.4 ms a frame for s100 on an A720 core (frame-eyes notes/e0-runtime.md).
"""
import json
import math
import os

import numpy as np

W, H = 512, 400


class Net:
    def __init__(self, folder):
        import cv2
        import ncnn
        self.cv2 = cv2
        self.folder = folder
        with open(os.path.join(folder, "model.json")) as f:
            self.info = json.load(f)
        self.name = str(self.info["name"])
        self.in_h, self.in_w = self.info["input"]
        self.scale = float(self.info.get("gaze_scale", 30.0))
        self.out = int(self.info.get("out", 6))
        self.in_name, self.out_name = self.info.get("in", "in0"), self.info.get("outname", "out0")
        net = ncnn.Net()
        net.opt.num_threads = 1
        net.opt.use_vulkan_compute = False
        net.opt.use_fp16_storage = net.opt.use_fp16_arithmetic = net.opt.use_fp16_packed = True
        for key, load in (("param", net.load_param), ("bin", net.load_model)):
            if load(os.path.join(folder, self.info[key])) != 0:
                raise RuntimeError(f"{folder}: can't load {self.info[key]}")
        self.ncnn, self.net = ncnn, net

    def __call__(self, img, cam):
        """img: one camera's 400x512 uint8 frame -> dict(gaze=(yaw, pitch), x, y, open, visible),
        open and visible as probabilities."""
        x = self.cv2.resize(img, (self.in_w, self.in_h), interpolation=self.cv2.INTER_AREA)
        x = x.astype(np.float32)[None] * (1.0 / 255.0)
        ex = self.net.create_extractor()
        ex.input(self.in_name, self.ncnn.Mat(x))
        o = np.array(ex.extract(self.out_name)[1]).reshape(-1)[cam * self.out:(cam + 1) * self.out]
        sig = lambda v: 1.0 / (1.0 + math.exp(-float(v)))  # noqa: E731
        return {"gaze": (float(o[0]) * self.scale, float(o[1]) * self.scale), "x": float(o[2]) * W,
                "y": float(o[3]) * H, "open": sig(o[4]), "visible": sig(o[5])}
