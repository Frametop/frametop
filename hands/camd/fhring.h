/*
 * fhring - the shared-memory frame ring fh-camd writes and trackers read.
 *
 * One file, /run/user/UID/framehands/cam-ring (FH_RING_NAME in the user's runtime
 * folder; the folder is private to the user), holds a header, then for each camera
 * a few slots, each a slot header followed by the image rows packed tightly
 * (stride == width for 8-bit mono). Only complete, bright frames are published.
 *
 * Writer, for frame n of a camera:  slot = n % nslots
 *   slot.seq = 2n+1; write slot fields and pixels; slot.seq = 2n+2; cam.latest = n
 * Reader:
 *   n = cam.latest; read slot.seq, expect 2n+2; copy; re-read slot.seq; if it
 *   changed the copy is torn, retry with the new latest.
 *
 * All multi-byte fields are little-endian; offsets are fixed so Python can read
 * them with struct (tools/ring.py mirrors this file).
 */

#pragma once

#include <assert.h>
#include <stdint.h>

#define FH_RING_MAGIC     "FHRING01"
#define FH_RING_VERSION   1
#define FH_RING_MAX_CAMS  8
#define FH_RING_SLOTS     4
#define FH_RING_NAME      "framehands/cam-ring"    /* in /run/user/UID */

enum {
    FH_FMT_GREY8 = 0,
};

enum {
    FH_CAM_DARK = 1u << 0,          /* the near-black exposures between this node's */
                                    /* normal frames (fh-camd --with-dark)          */
    FH_CAM_COLOR = 1u << 1,         /* an Arcturus color camera's luma, downscaled  */
                                    /* (fh-camd --with-color). Not synced with the  */
                                    /* mono cameras, and capture_ns is on its own   */
                                    /* clock: line it up with them by dqbuf_ns      */
};

typedef struct {
    char              sensor[32];   /* media entity, e.g. "og01a1b 4-0060"          */
    char              name[32];     /* calibration name if known, else sensor slug  */
    int32_t           node;         /* N of /dev/videoN                             */
    uint32_t          format;       /* FH_FMT_*                                     */
    uint32_t          width;
    uint32_t          height;
    uint32_t          stride;       /* bytes per row in the ring                    */
    uint32_t          nslots;
    uint64_t          slot_offset;  /* file offset of slot 0                        */
    uint64_t          slot_bytes;   /* slot header + image, 64-byte aligned         */
    volatile uint64_t latest;       /* newest published frame number, 0 = none yet  */
    uint64_t          published;    /* frames published                             */
    uint64_t          dropped;      /* dark, stale or torn frames not published     */
    uint32_t          flags;        /* FH_CAM_*                                     */
    float             dark_mean;    /* mono: mean luma of its latest near-black     */
                                    /* frame (a short fixed exposure, so it follows */
                                    /* the room's IR light, sunlight above all);    */
                                    /* 0 before the first                           */
    uint8_t           reserved[24];
} fh_ring_cam_t;                    /* 160 bytes */

typedef struct {
    volatile uint64_t seq;          /* 2n+1 while frame n is written, 2n+2 when done */
    uint64_t          frame;        /* n                                            */
    uint64_t          capture_ns;   /* V4L2 timestamp (camera clock)                */
    uint64_t          dqbuf_ns;     /* CLOCK_MONOTONIC when XRService dequeued it   */
    uint64_t          publish_ns;   /* CLOCK_MONOTONIC when the copy finished       */
    uint32_t          v4l2_seq;     /* V4L2 sequence number                         */
    float             mean;         /* mean luma on a sparse grid                   */
    uint8_t           reserved[16];
} fh_ring_slot_t;                   /* 64 bytes, image follows */

typedef struct {
    char              magic[8];     /* FH_RING_MAGIC                                */
    uint32_t          version;
    uint32_t          header_bytes; /* sizeof(fh_ring_hdr_t)                        */
    uint32_t          ncams;
    uint32_t          reserved0;
    uint64_t          file_bytes;
    int64_t           writer_pid;
    volatile uint64_t heartbeat_ns; /* CLOCK_MONOTONIC, refreshed at least every 0.2 s */
    uint8_t           reserved[16];
    fh_ring_cam_t     cams[FH_RING_MAX_CAMS];
} fh_ring_hdr_t;

static_assert(sizeof(fh_ring_cam_t) == 160, "fh_ring_cam_t layout");
static_assert(sizeof(fh_ring_slot_t) == 64, "fh_ring_slot_t layout");
static_assert(sizeof(fh_ring_hdr_t) == 64 + 160 * FH_RING_MAX_CAMS, "fh_ring_hdr_t layout");
