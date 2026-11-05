# Find Frametop's pointer dot (pure white, ~4 px, alone on a darker patch) in each frame.
# detect.py VIDEO OUT.csv [START END]
import cv2, numpy as np, sys, csv
cap = cv2.VideoCapture(sys.argv[1])
fps = cap.get(cv2.CAP_PROP_FPS)
a = float(sys.argv[3]) if len(sys.argv) > 3 else 0
b = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
cap.set(cv2.CAP_PROP_POS_MSEC, a * 1000)
out = csv.writer(open(sys.argv[2], "w"))
out.writerow(["t", "x", "y", "area", "ring"])
i = round(a * fps)
while i / fps <= b:
    ok, im = cap.read()
    if not ok: break
    mn = im.min(axis=2)
    m = (mn >= 225).astype(np.uint8)
    n, lab, st, cen = cv2.connectedComponentsWithStats(m)
    g = cv2.cvtColor(im, cv2.COLOR_BGR2GRAY)
    best = None
    for j in range(1, n):
        x, y, w, h, ar = st[j]
        if not (6 <= ar <= 40 and 2 <= w <= 7 and 2 <= h <= 7): continue
        if ar / (w * h) < 0.5 or not (0.5 <= w / h <= 2): continue
        x0, y0, x1, y1 = max(x - 7, 0), max(y - 7, 0), min(x + w + 7, im.shape[1]), min(y + h + 7, im.shape[0])
        patch = g[y0:y1, x0:x1].astype(np.float32)
        inner = np.zeros_like(patch, bool)
        inner[max(y - y0 - 2, 0):y - y0 + h + 2, max(x - x0 - 2, 0):x - x0 + w + 2] = True
        ring = patch[~inner]
        score = float(np.percentile(ring, 85))
        if score > 120: continue
        if best is None or score < best[4]:
            best = (i / fps, cen[j][0], cen[j][1], ar, score)
    if best: out.writerow([f"{best[0]:.3f}", f"{best[1]:.1f}", f"{best[2]:.1f}", best[3], f"{best[4]:.0f}"])
    i += 1
print("done", i)
