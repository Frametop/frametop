"""How many compiles or test files the test runners (testgate.py, ctests.py) run at once.

FT_TEST_JOBS sets it. Otherwise it is the CPUs this process may use (its CPU affinity, and the
tightest cpu.max quota on its cgroup or one above it), at most MAX_JOBS, and no more than the memory
available allows at MEM_PER_JOB each: a Frame in VR has a few GB free, and the biggest compile
(ft-together.cpp under the sanitizers) peaks near 460 MB. Leave it at 1 while someone wears the headset
(FT_TEST_JOBS=1, which `./ft dev test` passes into the image; `podman exec -e FT_TEST_JOBS=1`):
SteamVR needs the CPUs, and a container's processes aren't in the cgroup of whatever started
them, so a cap on the caller doesn't reach them.
"""
import math
import os

MAX_JOBS = 4
MEM_PER_JOB = 512 << 20


def _cgroup():
    """This process's cgroup v2 folder ("" for the root, as a container sees its own)."""
    try:
        with open("/proc/self/cgroup") as f:
            for line in f:
                if line.startswith("0::"):
                    return line[3:].strip().strip("/")
    except OSError:
        pass
    return ""


def _cpus(fs="/sys/fs/cgroup"):
    try:
        n = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        n = os.cpu_count() or 1
    path = _cgroup()
    while True:     # the cgroup and each one above it: the tightest quota holds
        try:
            with open(os.path.join(fs, path, "cpu.max")) as f:
                quota, period = f.read().split()
            if quota != "max":
                n = min(n, max(1, math.ceil(int(quota) / int(period))))
        except (OSError, ValueError):
            pass
        if not path:
            return n
        path = os.path.dirname(path)


def _mem_jobs():
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return max(1, int(line.split()[1]) * 1024 // MEM_PER_JOB)
    except (OSError, ValueError, IndexError):
        pass
    return MAX_JOBS


def jobs(env=os.environ):
    try:
        n = int(env.get("FT_TEST_JOBS", ""))
        if n > 0:
            return n
    except ValueError:
        pass
    return max(1, min(_cpus(), MAX_JOBS, _mem_jobs()))
