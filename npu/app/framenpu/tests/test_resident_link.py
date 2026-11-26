"""Linked resident execution: batch replay, seal/retire lifecycle and build checks."""

from pathlib import Path
import random
import shutil
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
from framenpu import Device, DeviceBusy, Job, RecoveryRequired
from framenpu.errors import InvalidModel
from framenpu.profile import PROFILE
import framenpu.resident_landmark as rg
from test_library import FakePower
from test_resident_landmark_lifecycle import AdminTransport


def record(kind, **fields):
    r = [0] * 32
    r[0] = kind
    r[7:10] = [1, 1, 1]
    for index, value in fields.items():
        r[int(index[1:])] = value
    return r


def program():
    """Every module and phase order of the pinned graph, repeated to 63 layers."""
    conv = record(1, r7=64, r9=64, r24=4, r26=16)
    stem = record(2, r7=24, r8=2, r9=40, r24=4, r26=16)
    inplace = record(3, r7=2, r8=3, r9=40, r23=1, r24=12, r25=5)
    depthwise = record(3, r7=2, r8=2, r9=33, r24=8)
    add = record(4, r7=3, r8=5, r9=7, r24=4)
    pool = record(5, r7=2, r8=2, r9=8, r24=4)
    average = record(5, r7=40, r23=2, r24=2)
    head = record(1, r18=2, r24=1, r26=16)
    return ([stem, conv, inplace, depthwise, add, pool, average, head] * 8)[:63]


def running():
    state = [0] * 32
    state[0], state[3], state[27], state[31] = 3, 63, 602112, rg.SEAL
    return state


def words_for(state, units, stop=(63, 0), fault=0):
    words = [0] * 32
    words[2], words[3], words[8] = 5, rg.BATCH, state[8] + 1
    words[19], words[23] = units, 96000
    words[24:26] = stop
    words[30] = fault
    return words


class BatchReplayTests(unittest.TestCase):
    def test_any_request_split_reaches_the_single_module_result(self):
        records = program()
        reference = running()
        total = 0
        while reference[0] == 3:
            layer = records[reference[19]]
            words = words_for(reference, 1)
            words[3] = rg._kind(reference, layer)
            reference = rg._transition(reference, words, 1, layer)
            total += 1
        self.assertEqual((reference[0], reference[19]), (2, 63))
        whole = rg._batch_transition(running(), words_for(running(), total), total, records)
        self.assertEqual(whole[10:29], reference[10:29])
        self.assertEqual(whole[30], total)
        chooser = random.Random(7)
        for _ in range(20):
            state, done = running(), 0
            while state[0] == 3:
                count = min(chooser.randint(1, 400), total - done)
                state = rg._batch_transition(state, words_for(state, count), count, records)
                done += count
            self.assertEqual((done, state[10:29]), (total, reference[10:29]))

    def test_stop_point_frame_end_and_fault_bound_the_reported_count(self):
        records = program()
        state = running()
        stop = rg._phase_end(state, records)
        self.assertEqual(stop, (0, 1))
        words = words_for(state, 100, stop)
        packed = rg._batch_transition(state, words, 16, records)
        self.assertEqual((packed[19], packed[24], packed[18]), (0, 1, 16))
        self.assertEqual(rg._batch_transition(state, words, 5, records)[18], 5)
        for count in (17, 0, 101):
            with self.subTest(count=count), self.assertRaises(ValueError):
                rg._batch_transition(state, words, count, records)
        faulted = rg._batch_transition(state, words_for(state, 9, fault=1), 1, records)
        self.assertEqual((faulted[0], faulted[18], faulted[24]), (4, 1, 0))
        with self.assertRaises(ValueError):
            rg._batch_transition(state, words_for(state, 9, fault=1), 2, records)
        last = running()
        last[19], last[20] = 62, 1
        self.assertEqual(rg._batch_transition(last, words_for(last, 5), 1, records)[0], 2)
        with self.assertRaises(ValueError):
            rg._batch_transition(last, words_for(last, 5), 2, records)

    def test_recurring_phase_stop_ends_after_one_position_block(self):
        records = program()
        state = running()
        state[19] = 1
        words = words_for(state, 1000, (1, 0))
        block = 16 + 2 + 64
        after = rg._batch_transition(state, words, block, records)
        self.assertEqual((after[19], after[24], after[22]), (1, 0, 32))
        with self.assertRaises(ValueError):
            rg._batch_transition(state, words, block + 1, records)


class FusedReplayTests(unittest.TestCase):
    def frame(self, fused):
        """Unit count and the state at every layer boundary of one frame."""
        records, state, units, boundaries = program(), running(), 0, []
        while state[0] == 3:
            layer = state[19]
            kind = rg._kind(state, records[layer])
            self.assertFalse(fused and kind == 8, "a fused frame has no post unit")
            rg._unit(state, kind, records[layer], 0, fused)
            units += 1
            if state[19] != layer:
                boundaries.append(state[10:29])
        return units, boundaries, state

    def test_fused_frame_skips_exactly_the_post_units(self):
        plain, plain_boundaries, plain_end = self.frame(False)
        fused, fused_boundaries, fused_end = self.frame(True)
        self.assertEqual(fused_boundaries, plain_boundaries)
        self.assertEqual((fused_end[0], fused_end[19]), (2, 63))
        post = 0
        for r in program():
            if r[0] in (1, 2):
                groups = r[8] if r[0] == 2 else -(-r[8] * r[9] // 32)
                groups *= -(-r[9] // 32) if r[0] == 2 else 1
                post += groups * r[7]
        self.assertEqual(plain - fused, post)

    def test_fused_group_ends_in_the_next_pack_phase_or_the_logit_phase(self):
        records = program()
        state = running()
        state[19], state[24] = 1, 1  # 1x1 layer, HMX phase of its first group
        words = words_for(state, 100, (1, 0))
        after = rg._batch_transition(state, words, 2, records, True)
        self.assertEqual((after[19], after[24], after[22], after[18]), (1, 0, 32, 0))
        with self.assertRaises(ValueError):
            rg._batch_transition(state, words, 3, records, True)
        head = running()
        head[19], head[24], head[20] = 7, 1, 0
        self.assertEqual(rg._phase_end(head, records, True), (7, 2))

    def test_fused_frame_runs_and_restores_through_the_host(self):
        t = LinkTransport(program())
        t.fused = True
        original = bytes(t.memory[rg.ARENA_START : rg.ARENA_END])
        with linked_owner(t, fused=True) as owner:
            self.assertTrue(owner.fused)
            begin_frame(owner)
            while owner.native[0] == 3:
                owner._command(5, rg.BATCH, units=rg.UNIT_LIMIT)
            self.assertEqual(owner.native[0], 2)
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_END]), original)
        self.assertFalse(t.poisoned)

    def test_wrong_fusion_flag_is_refused_as_a_replay_mismatch(self):
        t = LinkTransport(program())
        t.fused = True
        owner = linked_owner(t, fused=False).__enter__()
        begin_frame(owner)
        with self.assertRaises(RecoveryRequired):
            owner._drive((2, 0))
        owner.close()
        self.assertTrue(t.poisoned)


class LinkTransport(AdminTransport):
    """Native model: loader commands in the slot, batches in the sealed image."""

    def __init__(self, records):
        super().__init__()
        self.records = records
        self.chooser = random.Random(3)
        self.wrong_pc = self.skip_retire = False
        self.fused = False
        self.batches = []

    def wait_complete(self, seq, timeout):
        words = list(struct.unpack_from("<32I", self.memory, PROFILE.arena))
        state = list(struct.unpack_from("<32I", self.memory, rg.STATE))
        opcode = self.get(PROFILE.header + 36)
        op = words[2]
        if op in (0, 1, 2, 3, 6) and not (op == 6 and state[31]):
            if opcode != 31:
                raise AssertionError("loader command used the batch opcode")
            super().wait_complete(seq, timeout)
            # The loader always executes uncached and publishes no cache markers.
            struct.pack_into("<I", self.memory, PROFILE.arena + 128 + 124, 0xF0A92010)
            struct.pack_into("<2I", self.memory, PROFILE.status + 56, 0, 0)
            return
        self.requests.append(words)
        error = 0
        pc = 0xF0A92010
        accepted = state[4:8] == words[4:8] and words[8] == state[8] + 1 and opcode == 31
        if op == 4 and accepted:
            if not 0 < words[22] <= rg.INPUT_BYTES - state[27]:
                raise AssertionError("input upload exceeds the loader's bound")
            state[8:10], state[29:31] = words[8:10], [0, 0]
            state[27] += words[22]
        elif op in (7, 8) and accepted and words[21:23] == [rg.CODE, len(self.image)]:
            state[8:10], state[29:31] = words[8:10], [0, 0]
            state[31] = rg.SEAL if op == 7 else 0
        elif op == 5 and opcode == 32 and state[31] == rg.SEAL and words[3] == rg.BATCH:
            if bytes(self.memory[rg.CODE : rg.CODE + len(self.image)]) != self.image:
                raise AssertionError("batch entered an image that is not the sealed one")
            probe, allowed = list(state), 0
            while allowed < words[19]:
                layer = self.records[probe[19]]
                allowed += 1
                end = rg._unit(probe, rg._kind(probe, layer), layer, words[30], self.fused)
                if probe[0] != 3 or end and probe[19:25:5] == words[24:26]:
                    break
            count = self.chooser.randint(1, allowed)
            self.batches.append(count)
            state = rg._batch_transition(state, words, count, self.records, self.fused)
            cached = bool(self.get(PROFILE.header + 40)) != self.wrong_pc
            pc = (0x87000000 if cached else 0xF0000000) + rg.CODE + 16
            error = 2 if words[30] else 0
            if words[30]:
                struct.pack_into("<I", self.memory, self.records[words[18]][3], 0xDECAFBAD)
        else:
            error = 1
        struct.pack_into("<32I", self.memory, rg.STATE, *state)
        meta = words + state
        meta[31] = pc
        struct.pack_into("<64I", self.memory, PROFILE.arena + 128, *meta)
        for worker in range(3):
            row = [0] * 16
            row[:3] = [seq, worker, error if worker == 0 else 0]
            row[7], row[8] = rg.SSR[worker], 0x87000000
            row[10], row[12] = 100, 200
            if worker == 0 and error == 2:
                row[3], row[4] = 37, pc
            struct.pack_into("<16I", self.memory, PROFILE.status + 64 * worker, *row)


def linked_owner(
    transport,
    *,
    cached=True,
    budget_ms=5.0,
    loader="resident_graph_loader_link_v1",
    hold_power=False,
    fused=False,
):
    def power():
        made = FakePower(transport.events)
        transport.power = made
        return made

    def factory(**kwargs):
        device = Device(_transport=transport, _power_factory=power, **kwargs)
        device._direct = Mock(installed=True)
        return device

    records = program()
    for r in records:
        r[3] = rg.ACTIVATIONS
    plan = {
        "records": records,
        "program": bytes(rg.PROGRAM_BYTES),
        "packed_weights": bytes(rg.WEIGHT_BYTES),
        "roots": {},
        "allocations": {},
        "high_water_offset": 0x1FEF000,
    }
    image = bytes(range(1, 201)) * 4
    transport.image = image.ljust(rg.CODE_LAYOUTS.get(loader, rg.CODE_BYTES), b"\0")
    transport.records = records
    with (
        patch.object(rg, "resident_landmark_plan", return_value=plan),
        patch.object(rg, "specifications", return_value=[]),
    ):
        return rg.ResidentLandmark(
            SimpleNamespace(),
            rg.LinkedJobs(Job(bytes(2048), loader, True, True), image, fused),
            cached_code=cached,
            budget_ms=budget_ms,
            hold_power=hold_power,
            _device_factory=factory,
            _lease_factory=Mock,
        )


def power_events(transport):
    return [e for e in transport.events if e.startswith("power") or e == "abandon"]


def begin_frame(owner):
    owner._command(3)
    # One request carries the whole input; the eleven-module path keeps 32 KiB.
    with unittest.TestCase().assertRaises(ValueError):
        owner._command(4, offset=rg.ACTIVATIONS, data=bytes(rg.INPUT_BYTES + 128))
    owner._command(4, offset=rg.ACTIVATIONS, data=bytes(rg.INPUT_BYTES))


class LinkedLifecycleTests(unittest.TestCase):
    def test_contract_budget_and_reservation_are_checked_before_device_access(self):
        t = LinkTransport(program())
        for options in ({"loader": "resident_graph_admin_cache_v1"}, {"budget_ms": 26}):
            with self.subTest(options=options), self.assertRaises(InvalidModel):
                linked_owner(t, **options)
        with patch.object(rg, "CODE", rg.CODE - 8192), self.assertRaises(InvalidModel):
            linked_owner(t)
        with patch.object(rg, "resident_landmark_plan", return_value={}):
            with self.assertRaises(InvalidModel):
                rg.ResidentLandmark(SimpleNamespace(), {}, budget_ms=10, _lease_factory=Mock)
        self.assertFalse(t.events)

    def test_frame_runs_in_sealed_image_and_every_arena_byte_restores(self):
        t = LinkTransport(program())
        original = bytes(t.memory[rg.ARENA_START : rg.ARENA_END])
        with linked_owner(t) as owner:
            self.assertEqual(bytes(t.memory[rg.CODE : rg.CODE + rg.CODE_BYTES]), t.image)
            self.assertEqual(owner.native[31], rg.SEAL)
            begin_frame(owner)
            owner._drive((1, 0))
            self.assertEqual(owner.native[19:25:5], [1, 0])
            while owner.native[0] == 3:
                owner._command(5, rg.BATCH, units=rg.UNIT_LIMIT)
            self.assertEqual(owner.native[0], 2)
            rows = [c for c in owner.metrics["commands"] if c["op"] == 5]
            self.assertTrue(all(c["cache_cleanup"] == [0, 0] for c in owner.metrics["commands"]))
            self.assertTrue(all(c["pc"] >> 24 == 0x88 for c in rows))
            self.assertEqual(sum(c["count"] for c in rows), sum(t.batches))
        ops = [d[2] for d in t.requests]
        self.assertLess(ops.index(8), ops.index(6))
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_END]), original)
        self.assertFalse(t.poisoned)

    def test_changed_image_or_wrong_alias_retains_resources(self):
        for change in ("image", "alias"):
            t = LinkTransport(program())
            owner = linked_owner(t).__enter__()
            begin_frame(owner)
            if change == "image":
                t.memory[rg.CODE + 64] ^= 1
            else:
                t.wrong_pc = True
            count = len(t.batches)
            with self.subTest(change=change), self.assertRaises(RecoveryRequired):
                owner._command(5, rg.BATCH, units=8)
            owner.close()
            self.assertEqual(len(t.batches), count + (change == "alias"))
            self.assertTrue(t.poisoned)
            self.assertEqual(bytes(t.memory[rg.CONTROL : rg.CONTROL + 8]), rg.RECORD)

    def test_guarded_fault_steps_to_the_named_module_then_allows_only_close(self):
        t = LinkTransport(program())
        original = bytes(t.memory[rg.ARENA_START : rg.ARENA_END])
        owner = linked_owner(t, cached=False).__enter__()
        begin_frame(owner)
        with self.assertRaises(rg.JobFailed):
            owner._drive((1, 0), fault_kind=1)
        self.assertEqual((owner.native[0], owner.native[24], owner.invalid), (4, 1, True))
        self.assertEqual(owner.metrics["commands"][-1]["pc"] >> 24, 0xF1)
        owner.close()
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_END]), original)


class WideReservationTests(unittest.TestCase):
    def test_wide_loader_seals_checks_and_retires_sixteen_kibibytes(self):
        t = LinkTransport(program())
        original = bytes(t.memory[rg.ARENA_START : rg.ARENA_END])
        with linked_owner(t, loader="resident_graph_loader_link_wide_v1") as owner:
            self.assertEqual(owner.code_bytes, 16384)
            self.assertEqual(len(owner.image), 16384)
            self.assertLessEqual(rg.CODE + owner.code_bytes, rg.SCRATCH)
            self.assertEqual(bytes(t.memory[rg.CODE : rg.CODE + 16384]), t.image)
            begin_frame(owner)
            # A changed byte in the added page is refused before publication.
            t.memory[rg.CODE + 12288 + 64] ^= 1
            with self.assertRaises(RecoveryRequired):
                owner._command(5, rg.BATCH, units=8)
            t.memory[rg.CODE + 12288 + 64] ^= 1
        self.assertTrue(t.poisoned)
        t = LinkTransport(program())
        with linked_owner(t, loader="resident_graph_loader_link_wide_v1") as owner:
            begin_frame(owner)
            owner._command(5, rg.BATCH, units=8)
        sizes = [d[22] for d in t.requests if d[2] in (7, 8)]
        self.assertEqual(sizes, [16384, 16384])
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_END]), original)
        self.assertFalse(t.poisoned)

    def test_reservation_must_end_below_scratch_and_match_the_loader(self):
        self.assertEqual(rg.CODE + 16384, rg.SCRATCH)
        self.assertEqual(max(rg.CODE_LAYOUTS.values()), 16384)
        with patch.dict(rg.CODE_LAYOUTS, {"resident_graph_loader_link_wide_v1": 20480}):
            with self.assertRaises(InvalidModel):
                linked_owner(LinkTransport(program()), loader="resident_graph_loader_link_wide_v1")


class HeldPowerTests(unittest.TestCase):
    def test_default_powers_every_request_and_never_holds(self):
        t = LinkTransport(program())
        with linked_owner(t) as owner:
            begin_frame(owner)
            owner._command(5, rg.BATCH, units=8)
        events = power_events(t)
        self.assertEqual(events.count("power_on"), t.events.count("publish"))
        self.assertEqual(events.count("power_on"), events.count("power_off"))
        self.assertNotIn("power_hold", events)
        self.assertNotIn("power_verify", events)

    def test_held_power_starts_once_is_verified_per_request_and_stops_last(self):
        t = LinkTransport(program())
        original = bytes(t.memory[rg.ARENA_START : rg.ARENA_END])
        with linked_owner(t, hold_power=True) as owner:
            self.assertEqual(power_events(t)[:2], ["power_on", "power_hold"])
            begin_frame(owner)
            while owner.native[0] == 3:
                owner._command(5, rg.BATCH, units=rg.UNIT_LIMIT)
            self.assertEqual(power_events(t).count("power_on"), 1)
            self.assertNotIn("power_off", power_events(t))
        events = power_events(t)
        requests = t.events.count("publish")
        # One read-only verification per request, one more before power-off.
        self.assertEqual(
            events, ["power_on", "power_hold"] + ["power_verify"] * (requests + 1) + ["power_off"]
        )
        self.assertEqual(bytes(t.memory[rg.ARENA_START : rg.ARENA_END]), original)
        self.assertFalse(t.poisoned)
        self.assertFalse(t.owned)

    def test_changed_held_power_refuses_before_publication_and_retains_clocks(self):
        t = LinkTransport(program())
        owner = linked_owner(t, hold_power=True).__enter__()
        begin_frame(owner)
        published = t.events.count("publish")
        t.power.verify_error = RecoveryRequired("held HMX power configuration changed")
        with self.assertRaises(RecoveryRequired):
            owner._command(5, rg.BATCH, units=8)
        owner.close()
        self.assertEqual(t.events.count("publish"), published)
        self.assertEqual(power_events(t)[-1], "abandon")
        self.assertNotIn("power_off", power_events(t))
        self.assertTrue(t.poisoned)
        self.assertEqual(bytes(t.memory[rg.CONTROL : rg.CONTROL + 8]), rg.RECORD)

    def test_failed_open_before_ownership_still_powers_off(self):
        t = LinkTransport(program())
        owner = linked_owner(t, hold_power=True)
        with patch.object(owner, "_command", side_effect=DeviceBusy("gate busy")):
            with self.assertRaises(DeviceBusy):
                owner.__enter__()
        self.assertEqual(power_events(t), ["power_on", "power_hold", "power_verify", "power_off"])
        self.assertFalse(t.poisoned)


@unittest.skipUnless(
    all(shutil.which(tool) for tool in ("clang", "ld.lld", "llvm-objcopy", "llvm-objdump")),
    "LLVM tools are not installed",
)
class LinkedBuildTests(unittest.TestCase):
    def test_linked_build_verifies_and_leaves_cache_modules_unchanged(self):
        from framenpu.compiler import Toolchain
        from framenpu.errors import CompilerError

        try:
            Toolchain.discover()
        except CompilerError:
            self.skipTest("clang has no Hexagon target")
        import subprocess

        with tempfile.TemporaryDirectory() as folder:
            for arguments in (
                ["tools/build_resident_landmark.py", "--linked", "--output", folder],
                ["tools/check_resident_link.py", "--jobs", folder, "--output", folder + "/v.json"],
            ):
                subprocess.run(
                    [sys.executable, *arguments], cwd=ROOT, check=True, capture_output=True
                )
            loader, image, fused = rg.load_linked(folder)
            self.assertFalse(fused)
            self.assertLessEqual(len(loader.code), 4096)
            self.assertLessEqual(len(image), rg.CODE_BYTES)
            (Path(folder) / "resident_graph_linked.bin").write_bytes(image[:-4])
            with self.assertRaises(InvalidModel):
                rg.load_linked(folder)


class LinkVerifierTests(unittest.TestCase):
    def test_image_rejects_indirect_control_flow_and_self_addresses(self):
        from check_resident_link import verify_image

        base = 0xF0000000 + rg.CODE
        symbols = {"rg_batch": base, "rg_op_dw": base + 0x800}
        good = "f1ff0000: r0 = ##-0xe5dcf80\nf1ff0004: jumpr r31\nf1ff0008: if (p0) jumpr:nt r31"
        verify_image(good, symbols)
        for bad in (
            good + "\nf1ff000c: callr r2",
            good + "\nf1ff000c: jumpr r1",
            good + "\nf1ff000c: icinva(r0)",
            good + "\nf1ff000c: immext(#0xf1ff0800)",
            good + "\nf1ff000c: r1 = ##-0x7700f800",
        ):
            with self.subTest(bad=bad[-24:]), self.assertRaises(ValueError):
                verify_image(bad, symbols)
        with self.assertRaises(ValueError):
            verify_image(good, {"rg_batch": base + 4})


if __name__ == "__main__":
    unittest.main()
