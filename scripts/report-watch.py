#!/usr/bin/env python3
"""report-watch: what scripts/report.sh --watch records while the problem happens. On the Frame:
  python3 scripts/report-watch.py SECONDS AREA[,AREA...]
Each line: the time, the area, and what it saw. Areas (any of them, each in a thread of its own):
  system   every 5 s: the load, the CPU clocks, the hottest thermal zone, free memory, and the
           processes that used the most CPU in those 5 s
  desktop  ft-screens' debug line (the Steam menu, our keyboard, which laser drags what, where
           typing goes) whenever it changes
  gaze     the gaze service's status (which tracker, awake, samples coming in, the headset worn)
           whenever it changes
  hands    each second ft-hands publishes or stops: its frames per second, their latency, how often
           each hand was in them and how sure, the closest thumb-index and thumb-middle tips; and
           sides.json whenever the side cameras' decision changes
  voice    Frametop Voice's events, with what was said left out (its word count stays), and the
           microphone's level each second (peak and RMS, dBFS; no sound is kept)
Read-only: it asks the services over their sockets and reads their published files.
"""
import json
import math
import mmap
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from array import array
from pathlib import Path

RUN = Path(f"/run/user/{os.getuid()}")
lock = threading.Lock()


def say(area, text):
    now = time.time()
    with lock:
        print(time.strftime("%H:%M:%S", time.localtime(now)) + f".{int(now * 100) % 100:02d}", f"[{area}]", text, flush=True)


def ask(name, msg, timeout=1.0):
    """A request to one of Frametop's abstract datagram sockets; the reply, or None."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind("")
    s.settimeout(timeout)
    try:
        s.sendto(msg.encode(), "\0" + name)
        return s.recv(65536).decode()
    except OSError:
        return None
    finally:
        s.close()


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return ""


# ---- system


def cpu_times():
    """pid -> (name, utime + stime in ticks)."""
    out = {}
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            s = open(f"/proc/{d}/stat").read()
        except OSError:
            continue
        name, rest = s[s.index("(") + 1:s.rindex(")")], s[s.rindex(")") + 2:].split()
        out[int(d)] = (name, int(rest[11]) + int(rest[12]))
    return out


def system(end):
    tick = os.sysconf("SC_CLK_TCK")
    zones = sorted(Path("/sys/class/thermal").glob("thermal_zone*"))
    cpus = sorted(Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_cur_freq"),
                  key=lambda p: int(p.parent.parent.name[3:]))
    before, t0 = cpu_times(), time.monotonic()
    while time.monotonic() < end:
        time.sleep(min(5, max(0.1, end - time.monotonic())))
        now, t1 = cpu_times(), time.monotonic()
        used = sorted(((v[1] - before[p][1]) / tick / (t1 - t0) * 100, v[0]) for p, v in now.items()
                      if p in before and before[p][0] == v[0])
        top = ", ".join(f"{name} {pct:.0f}%" for pct, name in reversed(used[-5:]) if pct >= 1)
        before, t0 = now, t1
        temps = []
        for z in zones:
            t = read(z / "temp")
            if t.lstrip("-").isdigit():
                temps.append((int(t) / 1000, read(z / "type")))
        hot = max(temps) if temps else (float("nan"), "?")
        mhz = " ".join(str(int(read(c) or 0) // 1000) for c in cpus)
        mem = {l.split(":")[0]: int(l.split()[1]) for l in open("/proc/meminfo") if l.split(":")[0] in ("MemAvailable", "MemTotal")}
        say("system", f"load {read('/proc/loadavg').split(' ')[0]}  CPU MHz {mhz}  hottest {hot[0]:.1f} C ({hot[1]})  "
                      f"memory free {mem.get('MemAvailable', 0) // 1024} of {mem.get('MemTotal', 0) // 1024} MB  top: {top or '-'}")


# ---- desktop


def desktop(end):
    last = None
    while time.monotonic() < end:
        line = ask("ft_screens", "debug") or "no answer from ft-screens"
        if line != last:
            say("desktop", line.strip())
            last = line
        time.sleep(0.2)


# ---- gaze

GAZE_KEYS = ("tracker", "source", "awake", "idle", "ft_gaze", "own_running", "eyes_process", "headset_on",
             "eye_misses", "lessons", "calibration_samples")


def gaze(end):
    last = None
    while time.monotonic() < end:
        r = ask("ft_gazed", "status", 0.5)
        try:
            st = json.loads(r) if r else None
        except ValueError:
            st = None
        if st is None:
            text = "no answer from the gaze service (ft-gazed)"
        else:
            age = st.get("sample_age_s")
            samples = "none" if age is None else "coming in" if age < 0.5 else f"stopped ({age:.0f} s ago)" if age < 600 else "stopped"
            text = f"samples {samples}  " + "  ".join(f"{k} {json.dumps(st[k])}" for k in GAZE_KEYS if k in st)
        if text != last:
            say("gaze", text)
            last = text
        time.sleep(1)


# ---- hands (include/fh_hands.h)

HEAD = struct.Struct("<8sIIQQQII16x")          # magic, version, size, seq, capture_ns, publish_ns, nhands, ncapsules
HAND = struct.Struct("<IIff252xI")             # id, flags, confidence, reserved, pts[21][3], ncapsules: 272 bytes
V1_SIZE = HEAD.size + 2 * HAND.size + 64 * 32  # fh_hands_t
FIT_PINCH = 912                                # fh_hand_fit_t.pinch_d (thumb-index, thumb-middle), then fit_rms


def hands(end):
    path = RUN / "frametop-hands/hands"
    sides_last, quiet = None, None
    next_report = time.monotonic() + 1
    frames, seen, conf, lat, tips = 0, {0: 0, 1: 0}, {0: [], 1: []}, [], {0: [], 1: []}
    last_seq = None
    m, ino = None, None
    while time.monotonic() < end:
        # under its sequence lock: seq, a copy, seq again; the copy counts if both are the same even number
        data, again = b"", None
        try:
            st = path.stat()   # a restarted ft-hands writes a new file
            if m is None or (st.st_ino, st.st_size) != ino:
                with open(path, "rb") as f:
                    m, ino = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ), (st.st_ino, st.st_size)
            first = struct.unpack_from("<Q", m, 16)[0]
            data = bytes(m)
            again = struct.unpack_from("<Q", m, 16)[0]
        except (OSError, ValueError):
            m = None
        if len(data) >= V1_SIZE:
            magic, version, _, seq, cap, pub, n, _ = HEAD.unpack_from(data)
            if magic == b"FHHANDS1" and seq == first == again and seq % 2 == 0 and seq != last_seq and pub:
                new, last_seq = last_seq is not None, seq
            else:
                new = False
            if new:   # not the frame already there when the watch began
                frames += 1
                lat.append((time.monotonic_ns() - cap) / 1e6)
                for k in range(min(n, 2)):
                    _, flags, c, _, _ = HAND.unpack_from(data, HEAD.size + k * HAND.size)
                    side = flags & 1
                    seen[side] += 1
                    conf[side].append(c)
                    off = V1_SIZE + k * 1024
                    if version >= 2 and len(data) >= off + 1024 and struct.unpack_from("<I", data, off + 4)[0] & 1:
                        tips[side].append(struct.unpack_from("<fff", data, off + FIT_PINCH))
        if time.monotonic() >= next_report:
            next_report += 1
            if frames:
                lat.sort()
                def side(s, name):
                    if not seen[s]:
                        return f"{name} -"
                    t = tips[s]
                    fit = (f" closest thumb-index {1000 * min(x[0] for x in t):.0f} mm, thumb-middle "
                           f"{1000 * min(x[1] for x in t):.0f} mm, fit {1000 * sum(x[2] for x in t) / len(t):.1f} mm") if t else ""
                    return f"{name} {100 * seen[s] // frames}% conf {sum(conf[s]) / len(conf[s]):.2f}{fit}"
                say("hands", f"{frames} frames/s  age at read {lat[len(lat) // 2]:.0f} ms (median)  "
                             f"{side(0, 'left')}  |  {side(1, 'right')}")
                quiet = False
            elif quiet is not True:
                say("hands", "no new frames from ft-hands" + ("" if path.exists() else f" ({path} missing)"))
                quiet = True
            frames, seen, conf, lat, tips = 0, {0: 0, 1: 0}, {0: [], 1: []}, [], {0: [], 1: []}
            sides = read(RUN / "frametop-hands/sides.json")
            try:
                s = json.loads(sides)
                sides = f"side cameras {s.get('state')}, swapped {s.get('swapped')} (by {s.get('decided_by')})"
            except ValueError:
                sides = sides or "no sides.json"
            if sides != sides_last:
                say("hands", sides)
                sides_last = sides
        time.sleep(1 / 120)


# ---- voice

SAID = ("text", "matched", "ran")   # what was said, and the command it ran (its words)


def voice_events(end):
    path = RUN / "frametop-voice/control.sock"
    while time.monotonic() < end:
        s = socket.socket(socket.AF_UNIX)
        try:
            s.connect(str(path))
        except OSError as e:
            say("voice", f"Frametop Voice isn't running ({e.strerror})")
            s.close()
            while time.monotonic() < end and not path.exists():
                time.sleep(1)
            continue
        s.sendall(b"events\n")
        s.settimeout(0.5)
        buf = b""
        while time.monotonic() < end:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                chunk = b""
            if not chunk:
                say("voice", "Frametop Voice closed its events")
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue
                ev.pop("time", None)
                for k in SAID:
                    if isinstance(ev.get(k), str):
                        ev[k] = f"<{len(ev[k].split())} words>"
                    elif k in ev:
                        ev[k] = "<left out>"
                say("voice", json.dumps(ev))
        s.close()
        time.sleep(1)


def gain():
    """The mic's ALSA gain, which SteamVR's proxmicmute sets to 0 while nobody wears the headset."""
    vals = []
    for numid in (23, 24):   # VA_DEC0, VA_DEC1 on card 0
        try:
            out = subprocess.run(["amixer", "-c", "0", "cget", f"numid={numid}"], capture_output=True, text=True, timeout=3).stdout
        except (OSError, subprocess.SubprocessError):
            return "unknown"
        vals += [l.split("=", 1)[1] for l in out.splitlines() if ": values=" in l]
    if not vals:
        return "unknown"
    return " ".join(vals) + (" (muted by proxmicmute: the headset off)" if all(v == "0" for v in vals) else "")


def mic(end):
    rate = 16000
    try:
        p = subprocess.Popen(["pw-record", "--raw", "--rate", str(rate), "--channels", "1", "--format", "s16", "-"],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    except OSError as e:
        return say("voice", f"mic: can't run pw-record ({e})")
    silent = None
    try:
        while time.monotonic() < end:
            raw = p.stdout.read(rate * 2)
            if not raw:
                return say("voice", "mic: pw-record stopped")
            a = array("h", raw[: len(raw) // 2 * 2])
            peak = max(abs(x) for x in a) / 32768 if a else 0
            rms = math.sqrt(sum(x * x for x in a) / len(a)) / 32768 if a else 0
            if peak == 0:   # digital silence: once, then how long it lasted
                if silent is None:
                    say("voice", f"mic: digital silence (all zeros); its gain: {gain()}")
                    silent = time.monotonic()
                continue
            if silent is not None:
                say("voice", f"mic: silent for {time.monotonic() - silent:.0f} s")
                silent = None
            say("voice", f"mic peak {20 * math.log10(peak):.0f} dBFS, RMS {20 * math.log10(max(rms, 1e-9)):.0f} dBFS")
        if silent is not None:
            say("voice", f"mic: silent all the {time.monotonic() - silent:.0f} s since")
    finally:
        p.kill()


def main():
    if len(sys.argv) != 3 or not sys.argv[1].isdigit():
        sys.exit(__doc__)
    end = time.monotonic() + int(sys.argv[1])
    jobs = {"system": [system], "desktop": [desktop], "gaze": [gaze], "hands": [hands], "voice": [voice_events, mic]}
    areas = [a for a in sys.argv[2].split(",") if a]
    if not areas or any(a not in jobs for a in areas):
        sys.exit(f"areas: {', '.join(jobs)}")
    threads = [threading.Thread(target=f, args=(end,), daemon=True) for a in areas for f in jobs[a]]
    for t in threads:
        t.start()
    for t in threads:
        t.join(max(0, end - time.monotonic()) + 3)


if __name__ == "__main__":
    main()
