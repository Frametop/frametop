"""ft-npud's DSP job: one request's submit through framenpu, with the linked image's completion checks, and the
early answer (ready) from inside the completion validator. framenpu is imported when a Frames is made, so the
rest of the daemon can be tested without it."""
import os, struct, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'spikes/int8'))
import run_int8_link as RL


class Frames:
    """runs requests on the DSP: loader (the teacher-link loader job), image (the linked graph image), phases (a
    dict that sums framenpu's per-phase wall times, or None)"""

    def __init__(self, loader, image, phases=None):
        from framenpu import RecoveryRequired
        import hardware_teacher_link as TL
        self.loader, self.image, self.phases = loader, image, phases
        self.RecoveryRequired, self.TL = RecoveryRequired, TL
        # a split request's nonce, from a random start (RL.next_nonce steps it): worker 1 never takes an earlier
        # request's go, this run's or a previous daemon's, for this one's
        self.nonce = int.from_bytes(os.urandom(4), 'little')

    def __call__(self, lk, data, key=0, ready=None, crops=0):
        """One request on the DSP: (kernel ms, wall ms). With ready, framenpu's own after-the-acknowledge checks
        (the kernel statuses, then the image's metadata) also run in the completion validator, and ready(kernel ms)
        publishes the outputs and answers the client there: the acknowledge and restore (~0.6 ms) come after the
        client has its outputs. A failed check leaves ready uncalled and raises as before. crops: the request's crop
        count, for its timing kind (a 3- and a 4-crop split request run the same record counts)."""
        TL, phases = self.TL, self.phases
        # worker 0's records, worker 1's (0: a one-worker request); int8_graph.c ig_run's header words 1 and 4
        n, n1 = struct.unpack_from('<I', data, 4)[0], struct.unpack_from('<I', data, 16)[0]
        if n1:
            self.nonce = RL.next_nonce(self.nonce)
            data = RL.set_nonce(data, self.nonce)
        # the request kind Pace and Stamps time, a field each: record counts under 48, crops at most 4, the graph key
        kind = n + (n1 << 8) + (crops << 16) + (key << 24)
        meta_ok = lambda output: RL.meta_ok(output, n, n1)

        def validate(res):
            if lk.stamp is not None:
                lk.stamp.seen(res, kind)
            TL.linked_completion(res, len(self.image), output_bytes=256)
            lk._check_reservation(True)
            # never raise for these here (a validator's exception poisons the session): framenpu's status check
            # and the metadata check below raise after the acknowledge, as without ready
            if ready is None:
                return
            try:    # whatever goes wrong here is raised again after the acknowledge
                if not any(row[2] for row in res.workers) and meta_ok(res.output):
                    ready(res.kernel_reference_ticks / 19.2e3)
            except Exception as e:
                late.append(e)
        late = []
        t0 = time.perf_counter()
        if lk.pace is not None:
            lk.pace.arm(kind)
        ph = {} if phases is not None else None
        res = lk.dev._submit(self.loader, data, opcode=RL.IG_OPCODE, params=(1,), output_size=256, use_direct=True,
                             _completion_validator=validate, _prepare=lambda t: lk._check_reservation(True),
                             _held_power=lk.held, _phase_times=ph)
        if ph is not None:
            for k, v in ph.items():
                phases[k] = phases.get(k, 0.0) + v
        if late:
            raise late[0]
        if not meta_ok(res.output):
            raise self.RecoveryRequired('image metadata differs')
        return res.kernel_reference_ticks / 19.2e3, (time.perf_counter() - t0) * 1e3
