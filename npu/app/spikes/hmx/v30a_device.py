"""v30a_device.py --link DIR --crops F.u8 --ref REF.f32 --out OUT.json --run-on-device
      [--n N] [--rounds R] [--stop-after OP ...]

v30a on the DSP: the v30a-img link (build_v30a_link.py), sealed once into the 20 KiB
reservation, then resident replay requests of 1..3 crops (v30a_layout.build_plan). Root on
the Frame, with ft-npud stopped (this takes the DSP, the window and the reservation).

Protections are framenpu's tests/hardware_mercury.py ones (MIT, NatTheDyke), whose runner this
reuses: the lease, a refused run while a refusal record is present, audited direct dispatch, no
retry/recovery/restart, the window and reservation restored and the whole arena compared with
its starting hash. Resident requests add the session tag and check the residency status
(framenpu mercury_runtime._segment_metadata).

Checks, per crop: the stored tensor against v30a_graph.run's FP16 device contract (relative
max error), with --stop-after OP a truncated request ending at OP (bisect); the whole network's
keypoints (out0) against ncnn fp32 (REF from oracle/v30a_ref --fp32). Times: request wall and
per-crop body ticks over --rounds of 1, 2 and 3 crop requests. Writes statistics only.
"""
import argparse
import hashlib
import json
import os
import secrets
import struct
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1] / 'framenpu'
sys.path[:0] = [str(HERE), str(ROOT / 'src'), str(ROOT / 'tests'), str(ROOT / 'tools')]
from framenpu import Device, RecoveryRequired  # noqa: E402
from framenpu import _mercury_common as C  # noqa: E402
from framenpu.mercury_runtime import _segment_metadata  # noqa: E402
from framenpu.resident_gemm import _lease, RECORD  # noqa: E402
from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END  # noqa: E402
from hardware_hmx import PowerSnapshot, audited_device  # noqa: E402
from hardware_resident import settled_health  # noqa: E402
from hardware_landmark import service  # noqa: E402
from hmx_multi_run import failure_snapshot  # noqa: E402
import hardware_teacher_link as HL  # noqa: E402
from hardware_mercury import MercuryRunner  # noqa: E402

# the v30a image (17.4 KB) is a --code-kib 20 build: LinkRunner's seal, admin and reservation
# checks read these module constants at call time (framenpu mercury_runtime.reservation)
HL.CODE_BYTES = 0x5000
HL.SEAL_AT = HL.CODE + HL.CODE_BYTES - 128

import v30a_graph as G  # noqa: E402
import v30a_layout as L  # noqa: E402

OPTS = dict(replay=True, pw_group=8, resident=True, tiles=True, deep8=True, cbias=True, win_hi=C.WIN_HI_WIDE,
            compact=True)  # compact: the final store holds only the value blocks (k/ kind 4 with nj)
CHECK = dict(s2_direct=L.S2_PHASE, pw_group=8, resident=True, tiles=True, deep8=True, cbias=True,
             win_hi=C.WIN_HI_WIDE)
REF_WORDS = 672 * 49 + 128


class V30aRunner(MercuryRunner):
    """MercuryRunner's seal/admin/window access with resident replay requests (tag in words 5..8,
    word 9 = 1: slot 0)."""

    def __init__(self, dev, mem, loader, image):
        super().__init__(dev, mem, loader, image, profile=False, replay=True)
        self.tag = (0, 0, 0, 0)

    def install(self, mplan):
        """A new plan's resident images differ: a fresh tag makes the kernel copy them (status 3|4)
        on its first request. A kept tag would run the new plan on the last plan's images."""
        super().install(mplan)
        self.tag = (0, 0, 0, 0)
        while not self.tag[0]:
            self.tag = struct.unpack('<4I', secrets.token_bytes(16))

    def resident_request(self, mplan, crops):
        job = mplan.plan.jobs[0]
        data = bytearray(C.payload(mplan.plan, crops))
        struct.pack_into('<5I', data, 20, *self.tag, 1)
        sealed = self.sealed

        def validate(result):
            HL.linked_completion(result, len(self.image), output_bytes=C.META_BYTES)
            self._check_reservation(sealed)

        t0 = time.perf_counter()
        res = self.dev._submit(self.loader, bytes(data), opcode=C.RS_OPCODE, params=(1, job.first, job.end, 0, 0, 0),
                               output_size=C.META_BYTES, use_direct=True, _completion_validator=validate,
                               _prepare=lambda t: self._check_reservation(sealed))
        wall = (time.perf_counter() - t0) * 1e3
        # words 64..95: the kernel's FP16 overflow (exponent all ones) flags, one bit per lane, ORed
        # over every store. Recorded, not fatal: the tensors are compared below, which says where
        out = bytearray(res.output)
        flags = sum(bin(w).count('1') for w in struct.unpack_from('<32I', out, 256))
        out[256:384] = bytes(128)
        try:
            meta = _segment_metadata(bytes(out), crops, job.end - job.first, len(self.image), True,
                                     res.kernel_reference_ticks)
        except RecoveryRequired as e:
            # a check over the returned words only: the request completed cleanly (linked_completion),
            # so this is a plain failure and the harness still retires and restores
            m = struct.unpack('<256I', res.output)
            raise ValueError(f'{e}: crops {crops}, descriptors {job.end - job.first}, kernel ticks '
                             f'{res.kernel_reference_ticks}, words {[(i, hex(w)) for i, w in enumerate(m) if w]}')
        meta['op_end_ticks'] = list(struct.unpack_from(f'<{job.end - job.first}I', res.output, 384))  # last crop's
        return meta | {'overflow_lanes': flags, 'wall_ms': wall, 'wrapper_ms': res.kernel_reference_ticks / 19200}


def plan_for(model, stop_after=None):
    mp = L.build_plan(model, batch=3, stop_after=stop_after, **OPTS)
    for k in range(3):
        for row in struct.iter_unpack('<32I', mp.plan.table):
            if not L.native_ok(C.replay_row(row, k, mp.plan.crop_stride, mp.win_hi), **CHECK):
                raise ValueError(f'{stop_after or "full"}: a descriptor would be rejected')
    return mp


def stage(runner, mp, k, crop):
    cols = L.stem_columns(G.normalize(crop))
    L.stage_inputs(mp.crop(k), runner.put, {L.STEM_COLS: cols})


def read(runner, mp, k, strays=None):
    """strays: a list to record nonzero padding halves in instead of failing on them"""
    out, stray = (L.read_compact if OPTS['compact'] else L.read_tensor)(mp.crop(k), runner.get, mp.final)
    if strays is not None:
        strays.append(stray)
    elif stray:
        raise ValueError(f'{mp.final}: {stray} nonzero padding halves')
    return out


def resize(crop, size):
    """224 uint8 crop -> size (bilinear, pixel centres aligned), uint8"""
    if size == crop.shape[0]:
        return crop
    s = crop.shape[0] / size
    q = (np.arange(size) + 0.5) * s - 0.5
    i0 = np.clip(np.floor(q).astype(int), 0, crop.shape[0] - 1)
    i1 = np.clip(i0 + 1, 0, crop.shape[0] - 1)
    f = np.clip(q - i0, 0, 1)
    x = crop.astype(np.float64)
    rows = x[i0] * (1 - f)[:, None] + x[i1] * f[:, None]
    out = rows[:, i0] * (1 - f)[None] + rows[:, i1] * f[None]
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def rel_err(got, want):
    """got may carry v30a_layout's zero padding channels: they must be exactly 0"""
    if np.any(got[want.shape[0]:]):
        return float('inf')
    got = got[: want.shape[0]]
    return float(np.max(np.abs(got - want)) / (np.max(np.abs(want)) + 1e-12))


def run(runner, model, crops, ref, rounds, stops, report, dump=None, bisect_only=False):
    """fills report as it goes, so a failure keeps what ran"""
    report |= {'bisect': {}, 'frames': []}
    contract = [G.run(model, c, 'fp16', keep=True)[1] for c in crops]
    for stop in stops:  # truncated requests: the stored op output against the FP16 contract
        mp = plan_for(model, stop)
        runner.install(mp)
        for k in range(min(3, len(crops))):
            stage(runner, mp, k, crops[k])
        meta = runner.resident_request(mp, min(3, len(crops)))
        strays = []
        got = [read(runner, mp, k, strays) for k in range(min(3, len(crops)))]
        errs = [rel_err(g, contract[k][mp.final]) for k, g in enumerate(got)]
        if dump is not None:  # crop 0's device tensor and its FP16 contract, for a closer look
            np.save(dump / f'{stop}.dev.npy', got[0])
            np.save(dump / f'{stop}.ref.npy', contract[0][mp.final])
        report['bisect'][stop] = {'rel_err': errs, 'stray_halves': strays,
                                  'body_ms': meta['body_ms'], 'overflow_lanes': meta['overflow_lanes']}
    if bisect_only:
        return report
    mp = plan_for(model)
    runner.install(mp)
    strays = []
    finals, kp_err, pres_err, rel, copied, overflow = [None] * len(crops), [], [], [], [], []
    for i in range(0, len(crops), 3):  # accuracy: every crop once, three per request
        n = min(3, len(crops) - i)
        for k in range(n):
            stage(runner, mp, k, crops[i + k])
        meta = runner.resident_request(mp, n)
        copied.append(meta['params_copied'])
        overflow.append(meta['overflow_lanes'])
        for k in range(n):
            f = read(runner, mp, k, strays)
            finals[i + k] = f
            rel.append(rel_err(f, contract[i + k][G.FINAL]))
            outs = G.finish(model, f)
            r0, r1 = ref[i + k]
            kp_err.append(float(np.abs(outs['out0'] - r0).max()))
            pres_err.append(float(abs(outs['out1'][0] - r1)))
    report['accuracy'] = {'crops': len(crops), 'final_rel_err_max': max(rel),
                          'out0_vs_ref_max_px': max(kp_err), 'out0_vs_ref_median_px': float(np.median(kp_err)),
                          'presence_abs_max': max(pres_err), 'params_copied': copied,
                          'overflow_lanes': overflow, 'stray_halves': strays}
    sha = hashlib.sha256(np.asarray(finals[0], np.float32).tobytes()).hexdigest()
    for r in range(rounds):  # timing: 1, 2 and 3 crops, the first three crops again
        for n in (1, 2, 3):
            for k in range(n):
                stage(runner, mp, k, crops[k])
            meta = runner.resident_request(mp, n)
            same = hashlib.sha256(np.asarray(read(runner, mp, 0), np.float32).tobytes()).hexdigest() == sha
            report['frames'].append({'crops': n, 'wall_ms': meta['wall_ms'], 'body_ms': meta['body_ms'],
                                     'crop_body_ms': meta['crop_body_ms'], 'params_copied': meta['params_copied'],
                                     'op_end_ticks': meta['op_end_ticks'],
                                     'crop0_repeatable': same})
    report['accuracy']['ok'] = not any(strays)
    rows = report['frames'][3:] or report['frames']
    report['summary'] = {n: {'wall_ms_p50': float(np.median([f['wall_ms'] for f in rows if f['crops'] == n])),
                             'body_ms_p50': float(np.median([f['body_ms'] for f in rows if f['crops'] == n]))}
                         for n in (1, 2, 3)}
    report['summary']['repeatable'] = all(f['crop0_repeatable'] for f in report['frames'])
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run-on-device', action='store_true')
    for name in ('link', 'crops', 'out'):
        p.add_argument('--' + name, required=True, type=Path)
    p.add_argument('--ref', type=Path, help='oracle/v30a_ref --fp32 output (224 only); else the host fp32 graph')
    p.add_argument('--size', type=int, default=G.SIZE, help='input side: the 224 crops are resized to it')
    p.add_argument('--n', type=int, default=12)
    p.add_argument('--rounds', type=int, default=20)
    p.add_argument('--stop-after', nargs='*', default=[])
    p.add_argument('--dump', type=Path, help='a folder for the bisect tensors (crop 0)')
    p.add_argument('--bisect-only', action='store_true')
    a = p.parse_args()
    if not a.run_on_device or os.geteuid() != 0:
        p.error('Frame root and --run-on-device required')
    model = G.load(size=a.size)
    crops = [resize(c, a.size) for c in np.fromfile(a.crops, np.uint8).reshape(-1, 224, 224)[: a.n]]
    if a.ref is not None:
        if a.size != G.SIZE:
            p.error('--ref is the 224 ncnn oracle')
        r = np.fromfile(a.ref, np.float32).reshape(-1, REF_WORDS)[: a.n]
        ref = [(x[672 * 49: 672 * 49 + 63], x[672 * 49 + 63]) for x in r]
        ref_name = 'ncnn fp32'
    else:
        ref = [(o['out0'], o['out1'][0]) for o in (G.run(model, c, 'fp32') for c in crops)]
        ref_name = 'host fp32 graph'
    loader, image, record = HL.load_link(a.link)
    if (record.get('npu_hands', {}).get('ops') != 'stride-2 nk 2 (max pool), 3, 5' or record.get('s2_even')
            or record.get('reservation', {}).get('bytes') != HL.CODE_BYTES):
        p.error('--link must be a build_v30a_link.py image')
    for stop in a.stop_after + [None]:
        plan_for(model, stop)  # validate every plan before taking the device
    report = {'format': 'npu-hands-v30a-hmx-run', 'model_sha256': model.sha256, 'size': a.size, 'ref': ref_name, 'link': record['linked'],
              'loader': record['loader'], 'audits': {'restored_jobs': 0, 'restored_hmx_jobs': 0},
              'service_before': service()}
    failed, after = False, None
    lease = _lease()
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report['initial_health'] = settled_health(power, baseline)
            try:
                with Device() as observer:
                    mem = observer._transport.memory
                    if bytes(mem[CONTROL: CONTROL + 8]) == RECORD:
                        raise ValueError('resident refusal record present')
                    before = bytes(mem[ARENA_START:ARENA_END])
                # kept on disk too (root-only), so a run that ends unsafe can still be put back by hand
                fd = os.open(a.out.with_suffix('.arena-before.bin'), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                with os.fdopen(fd, 'wb') as fh:
                    fh.write(before)
                window = before[C.WIN_LO - ARENA_START: C.WIN_HI_WIDE - ARENA_START]
                reservation = before[HL.CODE - ARENA_START: HL.CODE + HL.CODE_BYTES - ARENA_START]
                retired, unsafe = True, False
                try:
                    with audited_device(power, report['audits'])(dispatch='direct') as dev:
                        runner = V30aRunner(dev, dev._transport.memory, loader, image)
                        try:
                            report['seal_wrapper_ms'] = runner.seal()
                            run(runner, model, crops, ref, a.rounds, a.stop_after, report, a.dump, a.bisect_only)
                        except RecoveryRequired:
                            unsafe = True
                            raise
                        finally:
                            if runner.sealed:
                                retired = False
                                if not unsafe and not dev._transport.poisoned:
                                    report['retire_wrapper_ms'] = runner.admin(8)
                                    retired = True
                except RecoveryRequired:
                    unsafe = True
                    raise
                finally:
                    report['window_restored'] = False
                    if not unsafe and retired:
                        with Device() as observer:
                            mem = observer._transport.memory
                            mem[C.WIN_LO: C.WIN_HI_WIDE] = window
                            mem[HL.CODE: HL.CODE + HL.CODE_BYTES] = reservation
                            after = bytes(mem[ARENA_START:ARENA_END])
                        report['window_restored'] = True
                if after != before:
                    raise ValueError('arena differs after window/reservation restoration')
                report['final_health'] = settled_health(power, baseline)
                report['service_after'] = service()
                report['all_checks_passed'] = True
            except BaseException as exc:
                failed = True
                report['error'] = f'{type(exc).__name__}: {exc}'
                report['after_failure'] = failure_snapshot(power)
    finally:
        lease.close()
        a.out.write_text(json.dumps(report, indent=1, default=str) + '\n')
    print(json.dumps({k: report.get(k) for k in ('all_checks_passed', 'error', 'bisect', 'accuracy', 'summary')},
                     indent=1, default=str), flush=True)
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
