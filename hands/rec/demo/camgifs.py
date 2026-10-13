#!/usr/bin/env python3
"""Recorded-guidance demo GIFs from a hand recorder session: the side cameras' own frames.

  frame-job --local -- PYTHON hands/rec/demo/camgifs.py SESSION_DIR [--out DIR] [--part ID]
                       [--sheets] [--view ID=PITCH,HFOV[,YAW]] [--lossy N]

PYTHON needs NumPy (frame-hands' .venv/bin/python has it). SESSION_DIR is a recorder session
(BASE/sessions/<id>) that ran the v2 script. Per demo clip of script-v2.json it takes the part's
right-hand step (the window mirrors it for the left hand, like the skeleton clips), or its
both-hands step, from the start of its prompt to the end of its last try; a redone step uses
the try that was kept.

The side cameras are fisheyes looking down and out, so each frame is drawn again as a plain
picture from between the eyes: a virtual camera PITCH degrees below straight ahead, HFOV wide,
filled from both side cameras (the session's calibration.json) for points 45 cm away, each
pixel from the camera that sees it nearer its centre. Clips run at the recording's 10 sets/s,
640x360 like the skeleton clips, grey, smoothed (steady), gifsicle's lossy pass when it's there
(HANDDEMO_GIFSICLE, or gifsicle on the PATH): about 1.4 MB for 10 s.
--sheets writes a contact sheet per clip instead (<id>.png, a frame every 2 s), to pick
--view per part before making the GIFs; views.json in the out folder keeps them
({"12": [-55, 100]}).

The GIFs show the room around the recorder: check them before they go anywhere public.
Writes OUT/<id>.gif (default hands/rec/demos-recorded, where the window's "Recorded" guidance
looks).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REC = os.path.dirname(HERE)
sys.path.insert(0, REC)
import takes  # noqa: E402  (hands/rec/takes.py)

GIFSICLE = os.environ.get("HANDDEMO_GIFSICLE") or shutil.which("gifsicle") or os.path.expanduser(
    "~/.cache/gifwork/gifsicle-src/src/gifsicle")

SCRIPT = os.path.join(REC, "script-v2.json")
OUT = os.path.join(REC, "demos-recorded")
W, H, SS = 640, 360, 2          # the clip, drawn at SS x SS and averaged down
PITCH, HFOV = -30.0, 100.0      # the default view: degrees below straight ahead, wide
DEPTH = 0.45                    # metres: where the two side cameras' pictures meet
FPS = 10                        # the recorder's RECORD_HZ
SIDES = ("slam_left", "slam_right")


def _pose(d, scale=1.0):
    """4x4 transform from a {plus_x, plus_z, position} pose (child axes in the parent frame)."""
    x = np.asarray(d["plus_x"], float)
    z = np.asarray(d["plus_z"], float)
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2] = x, np.cross(z, x), z
    m[:3, 3] = np.asarray(d["position"], float) * scale
    return m


class Fisheye:
    """A side camera: Kannala-Brandt intrinsics, posed in the head frame (OpenVR: +x right, +y up,
    -z forward) as hands/track does it, from a session's calibration.json: "cameras" are posed in
    the first camera's frame, "cad" holds every camera and the head in the headset's CAD frame."""

    def __init__(self, cal, name):
        i, c = next((i, c) for i, c in enumerate(cal["cameras"]) if c["sourceCamera"] == name)
        kb = next(k for k in c["intrinsics"] if k["cameraModel"] == "kb")
        self.fx, self.fy, self.cx, self.cy = kb["fx"], kb["fy"], kb["cx"], kb["cy"]
        self.k = [kb["k1"], kb["k2"], kb["k3"], kb["k4"]]
        self.width, self.height = c["width"], c["height"]
        head_from_cad = np.linalg.inv(_pose(cal["cad"]["__head"]))
        cad_from_cam0 = _pose(cal["cad"]["cameras"][0], 1e-3)
        head_from_cam = head_from_cad @ cad_from_cam0 @ _pose(c["extrinsics"], 1e-3)
        self.R, self.origin = head_from_cam[:3, :3], head_from_cam[:3, 3]

    def project(self, p):
        """Head-frame points (N,3) -> pixels (N,2) and the angle off the optical axis (N,), degrees."""
        q = (p - self.origin) @ self.R
        r = np.hypot(q[:, 0], q[:, 1])
        theta = np.arctan2(r, q[:, 2])
        t2 = theta * theta
        k1, k2, k3, k4 = self.k
        td = theta * (1 + t2 * (k1 + t2 * (k2 + t2 * (k3 + t2 * k4))))
        s = np.where(r > 1e-12, td / np.maximum(r, 1e-12), 0.0)
        uv = np.stack([self.fx * q[:, 0] * s + self.cx, self.fy * q[:, 1] * s + self.cy], axis=1)
        return uv, np.degrees(theta)


def view_maps(cams, pitch, hfov, yaw=0.0):
    """Per side camera: where each pixel of the virtual view samples it, and its weight."""
    w, h = W * SS, H * SS
    f = (w / 2) / np.tan(np.radians(hfov / 2))
    u, v = np.meshgrid(np.arange(w) - (w - 1) / 2, np.arange(h) - (h - 1) / 2)
    d = np.stack([u / f, -v / f, -np.ones_like(u)], -1).reshape(-1, 3)
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    a, b = np.radians(pitch), np.radians(yaw)
    rx = np.array([[1, 0, 0], [0, np.cos(a), -np.sin(a)], [0, np.sin(a), np.cos(a)]])
    ry = np.array([[np.cos(b), 0, -np.sin(b)], [0, 1, 0], [np.sin(b), 0, np.cos(b)]])
    p = d @ (ry @ rx).T * DEPTH
    maps, total = {}, 0
    for name, cam in cams.items():
        uv, ang = cam.project(p)
        inside = (uv[:, 0] >= 0) & (uv[:, 0] <= cam.width - 1) & (uv[:, 1] >= 0) & (uv[:, 1] <= cam.height - 1)
        wt = np.where(inside, np.clip((88 - ang) / 48, 0, 1) ** 3, 0)   # nearer the centre wins
        maps[name] = (uv[:, 0].reshape(h, w), uv[:, 1].reshape(h, w), wt.reshape(h, w))
        total = total + wt.reshape(h, w)
    return maps, np.maximum(total, 1e-6)


def sample(img, x, y):
    """Bilinear lookup of a grey image at float pixel positions."""
    x = np.clip(x, 0, img.shape[1] - 1.001)
    y = np.clip(y, 0, img.shape[0] - 1.001)
    x0, y0 = x.astype(np.int32), y.astype(np.int32)
    fx, fy = x - x0, y - y0
    a = img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx
    b = img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy


def render(maps, total, frames):
    acc = 0
    for name, (x, y, wt) in maps.items():
        acc = acc + sample(frames[name], x, y) * wt
    out = acc / total
    return out.reshape(H, SS, W, SS).mean(axis=(1, 3))


def demo_steps(script):
    """{demo id: [(section, hands), ...]} in the script's order, the step to use first: the
    right-hand one where the clip is a right hand's (mirrored for the left), else both hands."""
    steps = {}
    for sec in script["sections"]:
        for p in sec.get("prompts", []):
            if p.get("demo"):
                steps.setdefault(p["demo"], []).append((sec["id"], p.get("hands") or ""))
    rank = {"right": 0, "both": 1}
    return {d: sorted(s, key=lambda sh: rank.get(sh[1], 2)) for d, s in steps.items()}


def find_window(session, section, demo, hands):
    """(take dir, start ns, end ns) of the step's kept try, or None."""
    tdir = os.path.join(session, "takes")
    for name in sorted(os.listdir(tdir)):
        if not name.endswith("-" + section):
            continue
        take = os.path.join(tdir, name)
        prefix = "%s/%s/%s" % (section, demo, hands)
        t0 = t1 = None
        with open(os.path.join(take, "prompts.jsonl")) as f:
            for line in f:
                e = json.loads(line)
                ev = e.get("event")
                if ev == "prompt" and (e.get("id") == prefix or e.get("id", "").startswith(prefix + "/")):
                    t0, t1 = e["t"], None
                elif t0 is not None and t1 is None and ev in ("wait", "redo", "end", "pause"):
                    t1 = e["t"]
                    if ev == "redo":
                        t0 = None
        if t0 is not None:
            return take, t0, t1
    return None


def clip_sets(take, t0, t1):
    ti = takes.TakeIndex(take)
    deleted = takes.read_json(os.path.join(take, "take.json")).get("deleted") or []
    keep = [i for i in range(len(ti)) if t0 <= ti.time_ns(i) <= (t1 or 1 << 62)
            and not any(a <= ti.time_ns(i) <= b for a, b in deleted)]
    return ti, keep


def frames_of(ti, i):
    out = {}
    for c in ti.read_set(i):
        if c["name"] in SIDES:
            out[c["name"]] = np.frombuffer(c["pixels"], np.uint8).reshape(c["height"], c["width"]).astype(np.float32)
    return out


def encode(clip, out, lossy, sheet):
    """Grey uint8 frames (N,H,W) -> a looping GIF, or a contact sheet PNG."""
    raw_in = ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray", "-s", "%dx%d" % (W, H),
              "-r", str(FPS), "-i", "-"]
    if sheet:
        n = len(clip[::2 * FPS])
        cols = min(4, n)
        rows = (n + cols - 1) // cols
        cmd = raw_in + ["-vf", "select=not(mod(n\\,%d)),scale=320:-2,tile=%dx%d" % (2 * FPS, cols, rows),
                        "-frames:v", "1", out]
        subprocess.run(cmd, input=clip.tobytes(), check=True)
        return
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "raw.gif")
        cmd = raw_in + ["-vf", "split[a][b];[a]palettegen=max_colors=48:stats_mode=full[p];"
                        "[b][p]paletteuse=dither=none:diff_mode=rectangle", "-loop", "0", raw]
        subprocess.run(cmd, input=clip.tobytes(), check=True)
        if os.access(GIFSICLE, os.X_OK):
            subprocess.run([GIFSICLE, "-O3", "--lossy=%d" % lossy, raw, "-o", out], check=True)
        else:
            os.replace(raw, out)


def steady(clip, hold=12.0):
    """Less sensor noise, for a GIF that compresses (10-15 MB a clip without it, about 1.5 with):
    two [1 2 1] blurs, then a pixel that changed by less than hold grey levels keeps its last
    value, so the still parts of a frame repeat the one before."""
    c = clip.astype(np.float32)
    for _ in range(2):
        c[:, 1:-1] = (c[:, :-2] + 2 * c[:, 1:-1] + c[:, 2:]) / 4
        c[:, :, 1:-1] = (c[:, :, :-2] + 2 * c[:, :, 1:-1] + c[:, :, 2:]) / 4
    for i in range(1, len(c)):
        same = np.abs(c[i] - c[i - 1]) < hold
        c[i][same] = c[i - 1][same]
    return c


def stretch(clip):
    """Levels per clip: the IR frames are dim and flat."""
    lo, hi = np.percentile(clip[::max(1, len(clip) // 20)], (1, 99.7))
    x = np.clip((clip - lo) / max(hi - lo, 1), 0, 1) ** 0.8
    return (x * 255 + 0.5).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("session")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--part", help="one demo clip id (e.g. 7a)")
    ap.add_argument("--sheets", action="store_true", help="contact sheets instead of GIFs")
    ap.add_argument("--view", action="append", default=[], metavar="ID=PITCH,HFOV[,YAW]")
    ap.add_argument("--lossy", type=int, default=70, help="gifsicle's lossy level")
    a = ap.parse_args()
    with open(SCRIPT) as f:
        script = json.load(f)
    with open(os.path.join(a.session, "calibration.json")) as f:
        cal = json.load(f)
    cams = {n: Fisheye(cal, n) for n in SIDES}
    os.makedirs(a.out, exist_ok=True)
    views = takes.read_json(os.path.join(a.out, "views.json")) or {}
    for v in a.view:
        part, _, vals = v.partition("=")
        views[part] = [float(x) for x in vals.split(",")]
    if a.view:
        with open(os.path.join(a.out, "views.json"), "w") as f:
            json.dump(views, f, indent=1, sort_keys=True)
    cache = {}
    for demo, steps in demo_steps(script).items():
        if a.part and demo != a.part:
            continue
        found = next(((s, h, w) for s, h in steps for w in [find_window(a.session, s, demo, h)] if w), None)
        if not found:
            print("%-4s not in this session" % demo)
            continue
        section, hands, (take, t0, t1) = found
        ti, keep = clip_sets(take, t0, t1)
        if not keep:
            print("%-4s %s/%s: no sets" % (demo, section, hands))
            continue
        view = tuple(views.get(demo) or (PITCH, HFOV))
        if view not in cache:
            cache[view] = view_maps(cams, *view)
        maps, total = cache[view]
        clip = stretch(steady(np.stack([render(maps, total, frames_of(ti, i)) for i in keep])))
        out = os.path.join(a.out, demo + (".png" if a.sheets else ".gif"))
        encode(clip, out, a.lossy, a.sheets)
        print("%-4s %-12s %-5s %5.1f s  view %s  %.2f MB" % (demo, section, hands, len(keep) / FPS,
                                                          ",".join("%g" % x for x in view),
                                                          os.path.getsize(out) / 1e6), flush=True)


if __name__ == "__main__":
    main()
