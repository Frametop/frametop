"""A kernel is a trusted, freestanding DSP executable, never a model weight file."""

from dataclasses import dataclass
from pathlib import Path
import hashlib
import json
import struct
from .profile import PROFILE, JOB_ABI


@dataclass(frozen=True)
class Job:
    code: bytes
    name: str = "custom"
    requires_hmx: bool = False
    supports_direct: bool = False

    def __post_init__(self):
        if not isinstance(self.code, bytes) or not 0 < len(self.code) <= PROFILE.code_size:
            raise ValueError("kernel must contain 1..4096 bytes linked at 0xf0a92000")
        if len(self.code) % 4:
            raise ValueError("kernel must end on a Hexagon word boundary")
        if not isinstance(self.name, str) or not 0 < len(self.name) <= 128:
            raise ValueError("kernel name must contain 1..128 characters")
        if type(self.requires_hmx) is not bool:
            raise ValueError("requires_hmx must be boolean")
        if type(self.supports_direct) is not bool:
            raise ValueError("supports_direct must be boolean")

    def save(self, path):
        meta = json.dumps(
            {
                "abi": JOB_ABI,
                "name": self.name,
                "requires_hmx": self.requires_hmx,
                "supports_direct": self.supports_direct,
                "sha256": hashlib.sha256(self.code).hexdigest(),
            },
            sort_keys=True,
        ).encode()
        Path(path).write_bytes(b"FJOB" + struct.pack("<I", len(meta)) + meta + self.code)

    @classmethod
    def load(cls, path):
        with open(path, "rb") as stream:
            raw = stream.read(16385)
        if len(raw) > 16384 or raw[:4] != b"FJOB" or len(raw) < 8:
            raise ValueError("invalid FJOB container")
        (size,) = struct.unpack_from("<I", raw, 4)
        if not 0 < size <= 8192 or 8 + size >= len(raw):
            raise ValueError("invalid FJOB metadata length")
        try:
            meta = json.loads(raw[8 : 8 + size])
        except (ValueError, UnicodeError) as exc:
            raise ValueError("invalid FJOB JSON metadata") from exc
        if (
            not isinstance(meta, dict)
            or not {"abi", "name", "requires_hmx", "sha256"} <= meta.keys()
            or type(meta["abi"]) is not int
        ):
            raise ValueError("invalid FJOB metadata schema")
        code = raw[8 + size :]
        if meta.get("abi") != JOB_ABI or meta.get("sha256") != hashlib.sha256(code).hexdigest():
            raise ValueError("kernel ABI/checksum mismatch")
        return cls(code, meta["name"], meta["requires_hmx"], meta.get("supports_direct", False))


@dataclass(frozen=True)
class JobResult:
    output: bytes
    elapsed_ms: float
    workers: tuple

    @property
    def kernel_reference_ticks(self):
        """Worker-0 frame_job duration in raw c31:30 reference ticks, if recorded."""
        if not self.workers or len(self.workers[0]) < 14:
            return None
        row = self.workers[0]
        started = row[10] | row[11] << 32
        ended = row[12] | row[13] << 32
        return (ended - started) & 0xFFFFFFFFFFFFFFFF if started and ended else None
