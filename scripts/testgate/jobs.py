"""How many compiles or test files the test runners (testgate.py, ctests.py) run at once.

FT_TEST_JOBS sets it. Otherwise it is the CPUs this process may use (its CPU affinity, and its
cgroup's cpu.max quota when there is one), at most MAX_JOBS, and no more than the memory
available allows at MEM_PER_JOB each: a Frame in VR has a few GB free, and the biggest compile
(ft-together.cpp under the sanitizers) peaks near 460 MB. Leave it at 1 while someone wears the headset
(FT_TEST_JOBS=1): SteamVR needs the CPUs, and a container's processes aren't in the cgroup of
whatever started them, so a cap on the caller doesn't reach them.
"""
import math
import os

MAX_JOBS = 4
MEM_PER_JOB = 512 << 20


def _cpus():
    try:
        n = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        n = os.cpu_count() or 1
    try:
        with open("/sys/fs/cgroup/cpu.max") as f:
            quota, period = f.read().split()
        if quota != "max":
            n = min(n, max(1, math.ceil(int(quota) / int(period))))
    except (OSError, ValueError):
        pass
    return n


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
