# Render Frametop Tour clips from the 2026-10-20 narrated demo recording.
# render.py NAME... (or "all"); clips.json has the specs. Frames -> out/NAME/, then GIF -> gifs/NAME.gif
#
# A clip: {"scenes": [{"a": s, "b": s, "speed": 1, "zoom": 1, "cx": 960, "cy": 540, "follow": false,
#                     "ring": false, "caps": [[t, "text"], ...]}], "hold": s}
# caps times are source seconds; each caption shows from its time until the next one.
import csv, json, os, shutil, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(HERE, "src/VR 2026-10-20 21-31-57.mp4")
W = int(os.environ.get("GW", 640)); H = W * 9 // 16
FPS = int(os.environ.get("GFPS", 10))
DN = os.environ.get("GDN", "3:3:8:8")
S = W / 640
MAGENTA = (255, 62, 165)
CYAN = (56, 225, 255)
FONT = os.path.join(HERE, "fonts/NotoSans-%s.ttf")


def font(weight, size):
    return ImageFont.truetype(FONT % weight, size)


F_CAP = font("Bold", round(17 * S))
F_STEP = font("Bold", round(11 * S))


def load_dots():
    dots = []
    with open(os.path.join(HERE, "dots.csv")) as f:
        for r in csv.DictReader(f):
            if float(r["ring"]) <= 50:
                dots.append((float(r["t"]), float(r["x"]), float(r["y"])))
    return np.array(dots)


DOTS = load_dots()


def dot_track(a, b, n):
    """The pointer's position at n evenly spaced output times in a..b (source seconds), or None
    where it isn't seen within 0.25 s. Outliers (a lone jump) are dropped first."""
    sel = DOTS[(DOTS[:, 0] >= a - 0.5) & (DOTS[:, 0] <= b + 0.5)]
    keep = []
    for i in range(len(sel)):
        lo, hi = max(i - 3, 0), min(i + 4, len(sel))
        near = sel[lo:hi]
        d = np.hypot(near[:, 1] - sel[i, 1], near[:, 2] - sel[i, 2])
        if (d < 60).sum() >= min(3, len(near)):  # agrees with some neighbours
            keep.append(sel[i])
    sel = np.array(keep) if keep else np.zeros((0, 3))
    out = []
    for k in range(n):
        t = a + (b - a) * k / max(n - 1, 1)
        if len(sel) == 0:
            out.append(None); continue
        j = np.argmin(np.abs(sel[:, 0] - t))
        out.append((sel[j, 1], sel[j, 2]) if abs(sel[j, 0] - t) < 0.25 else None)
    return out


def smooth_path(pts, default, alpha):
    """A camera path: follows pts (None = hold), exponentially smoothed."""
    cur = None
    out = []
    first = next((p for p in pts if p is not None), default)
    cur = np.array(first, float)
    for p in pts:
        if p is not None:
            cur = cur * (1 - alpha) + np.array(p) * alpha
        out.append(cur.copy())
    return out


def extract(a, b, speed):
    vf = f"hqdn3d={DN},setpts=(PTS-STARTPTS)/{speed},fps={FPS}"
    cmd = ["ffmpeg", "-nostdin", "-v", "error", "-ss", f"{a}", "-t", f"{b - a}", "-i", SRC, "-an", "-vf", vf,
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    raw = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
    return list(np.frombuffer(raw, np.uint8).reshape(-1, 1080, 1920, 3))


def crop(fr, zoom, cx, cy):
    cw, ch = 1920 / zoom, 1080 / zoom
    x0 = min(max(cx - cw / 2, 0), 1920 - cw)
    y0 = min(max(cy - ch / 2, 0), 1080 - ch)
    im = Image.fromarray(fr).crop((int(x0), int(y0), int(x0 + cw), int(y0 + ch))).resize((W, H), Image.LANCZOS)
    return im, (x0, y0, cw / W)


def caption(layer, step, text, alpha):
    d = ImageDraw.Draw(layer)
    tw = max(d.textlength(text, font=F_CAP), d.textlength(step, font=F_STEP) if step else 0)
    ph = round((48 if step else 34) * S)
    pw = int(tw + 30 * S)
    x, y = round(12 * S), int(H - 12 * S - ph)
    box = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    bd = ImageDraw.Draw(box)
    bd.rounded_rectangle((x, y, x + pw, y + ph), round(9 * S), fill=(9, 12, 22, int(215 * alpha)))
    bd.rectangle((x + round(8 * S), y + round(8 * S), x + round(10 * S), y + ph - round(8 * S)),
                 fill=(*MAGENTA, int(255 * alpha)))
    ty = y + 6 * S
    if step:
        bd.text((x + 18 * S, ty), step, font=F_STEP, fill=(*CYAN, int(255 * alpha)))
        ty += 15 * S
    bd.text((x + 18 * S, ty), text, font=F_CAP, fill=(255, 255, 255, int(255 * alpha)))
    layer.alpha_composite(box)


def ring(layer, x, y, alpha):
    d = ImageDraw.Draw(layer)
    r = 11 * S
    d.ellipse((x - r, y - r, x + r, y + r), outline=(*MAGENTA, int(235 * alpha)), width=max(2, round(2.5 * S)))


def scene_frames(sc, step_of, first):
    a, b, speed = sc["a"], sc["b"], sc.get("speed", 1)
    zoom = sc.get("zoom", 1)
    frames = extract(a, b, speed)
    n = len(frames)
    pts = dot_track(a, b, n) if (sc.get("follow") or sc.get("ring")) else [None] * n
    if sc.get("follow"):
        cam = smooth_path(pts, (sc.get("cx", 960), sc.get("cy", 540)), sc.get("follow_alpha", 0.12))
    else:
        cam = [np.array((sc.get("cx", 960), sc.get("cy", 540)))] * n
    caps = sc.get("caps", [])
    out = []
    for k, fr in enumerate(frames):
        t = a + (b - a) * k / max(n - 1, 1)
        im, (x0, y0, sc_) = crop(fr, zoom, *cam[k])
        base = im.convert("RGBA")
        layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        if sc.get("ring") and pts[k] is not None:
            ring(layer, (pts[k][0] - x0) / sc_, (pts[k][1] - y0) / sc_, 1.0)
        cur = None
        for ci, (ct, text) in enumerate(caps):
            if t >= ct:
                cur = (ci, ct, text)
        if cur:
            ci, ct, text = cur
            # the clip's first caption is whole from its first frame (the still before it plays)
            fade = 1.0 if (first and ci == 0) else min(1.0, (t - ct) / (0.25 * speed) + 0.25)
            caption(layer, step_of(text), text, fade)
        base.alpha_composite(layer)
        out.append(np.array(base.convert("RGB")))
    return out


def render(name, spec):
    allcaps = [c[1] for sc in spec["scenes"] for c in sc.get("caps", [])]
    multi = len(allcaps) > 1 and not spec.get("nosteps") and not os.environ.get("GNOSTEPS")
    step_of = (lambda text: f"{allcaps.index(text) + 1} OF {len(allcaps)}") if multi else (lambda text: "")
    frames = []
    for i, sc in enumerate(spec["scenes"]):  # hard cuts: a crossfade doubled the captions
        frames.extend(scene_frames(sc, step_of, i == 0))
    frames.extend([frames[-1]] * round(spec.get("hold", 0.8) * FPS))
    od = os.path.join(HERE, "out", name)
    shutil.rmtree(od, ignore_errors=True)
    os.makedirs(od)
    for j, fr in enumerate(frames):
        Image.fromarray(fr).save(f"{od}/{j:04d}.png", compress_level=1)
    os.makedirs(os.path.join(HERE, "gifs"), exist_ok=True)
    if os.environ.get("GFMT") == "webp":  # animated WebP: Qt plays it like a GIF, a fraction of the size
        webp = os.path.join(HERE, "gifs", name + ".webp")
        pngs = sorted(p for p in os.listdir(od) if p.endswith(".png"))
        subprocess.run(["img2webp", "-loop", "0", "-lossy", "-q", os.environ.get("GQ", "55"), "-m", "6",
                        "-d", str(round(1000 / FPS)), *[os.path.join(od, p) for p in pngs], "-o", webp],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        print(f"{name}: {len(frames)} frames, {len(frames) / FPS:.1f} s, {os.path.getsize(webp) / 1e6:.2f} MB")
        return
    gif = os.path.join(HERE, "gifs", name + ".gif")
    pal = os.path.join(od, "pal.png")
    colors = spec.get("colors", int(os.environ.get("GCOLORS", 160)))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", str(FPS), "-i", f"{od}/%04d.png",
                    "-vf", f"palettegen=max_colors={colors}:stats_mode=diff", pal], check=True)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", str(FPS), "-i", f"{od}/%04d.png", "-i", pal,
                    "-lavfi", "paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle", "-loop", "0",
                    gif + ".raw.gif"], check=True)
    subprocess.run(["gifsicle", "-O3", f"--lossy={spec.get('lossy', int(os.environ.get('GLOSSY', 70)))}", gif + ".raw.gif", "-o", gif], check=True)
    os.remove(gif + ".raw.gif")
    print(f"{name}: {len(frames)} frames, {len(frames) / FPS:.1f} s, {os.path.getsize(gif) / 1e6:.2f} MB")


def main():
    with open(os.path.join(HERE, "scripts/clips.json")) as f:
        clips = json.load(f)
    names = list(clips) if sys.argv[1:] == ["all"] else sys.argv[1:]
    for n in names:
        render(n, clips[n])


if __name__ == "__main__":
    main()
