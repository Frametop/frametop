"""The hand tracker's capture formats, for Python: ft-camd's frame ring (hands/camd/fhring.h)
and the FHSET01 recordings (hands/track/record.h). recorder/tests/test_fhformat.py checks the
sizes and field offsets here against the C headers.

Standard library only; runs on Linux and Windows (the maintainer's PC)."""
import os
import struct

# ---------------------------------------------------------------- ft-camd's ring (fhring.h)

RING_MAGIC = b"FHRING01"
RING_VERSION = 1
RING_MAX_CAMS = 8
RING_NAME = "frametop-hands/cam-ring"   # in /run/user/UID
FMT_GREY8 = 0
CAM_DARK, CAM_COLOR = 1 << 0, 1 << 1    # fh_ring_cam_t.flags

# fh_ring_hdr_t without its cams: magic, version, header_bytes, ncams, reserved0, file_bytes,
# writer_pid, heartbeat_ns
RING_HDR = struct.Struct("<8sIIIIQqQ16x")                 # 64 bytes, then RING_MAX_CAMS cams
# fh_ring_cam_t: sensor, name, node, format, width, height, stride, nslots, slot_offset,
# slot_bytes, latest, published, dropped, flags, dark_mean
RING_CAM = struct.Struct("<32s32siIIIIIQQQQQIf24x")       # 160 bytes
# fh_ring_slot_t: seq, frame, capture_ns, dqbuf_ns, publish_ns, v4l2_seq, mean
RING_SLOT = struct.Struct("<QQQQQIf16x")                  # 64 bytes, the image follows

RING_HEARTBEAT_OFF = 40          # fh_ring_hdr_t.heartbeat_ns
RING_CAM_LATEST_OFF = 104        # fh_ring_cam_t.latest
RING_CAM_PUBLISHED_OFF = 112     # fh_ring_cam_t.published
RING_CAM_DROPPED_OFF = 120       # fh_ring_cam_t.dropped
RING_CAM_FLAGS_OFF = 128         # fh_ring_cam_t.flags
RING_CAM_DARK_MEAN_OFF = 132     # fh_ring_cam_t.dark_mean


def ring_path():
    """The ring's file: /run/user/UID/frametop-hands/cam-ring."""
    return "/run/user/%d/%s" % (os.getuid(), RING_NAME)


def ring_cam_offset(index):
    """Where camera `index`'s fh_ring_cam_t starts in the file."""
    return RING_HDR.size + index * RING_CAM.size


# ---------------------------------------------------------------- FHSET01 (record.h)

SET_MAGIC = b"FHSET01"           # 7 bytes; the header's magic field is 8 ("FHSET01\0")
SET_HDR = struct.Struct("<8sII")     # fh_set_hdr_t: magic, ncams, bytes (the whole record)
SET_CAM = struct.Struct("<16sIIQQ")  # fh_set_cam_t: name, width, height, capture_ns, dqbuf_ns
