/*
 * fh_calib - the tracking cameras' calibration, for the images in ft-camd's ring (camd/fhring.h),
 * so a reader of the ring needs neither the headset's factory files nor to work out which ring
 * camera is which. /run/user/UID/frametop-hands/calib, next to the ring, with the hands file's
 * sequence lock (read seq, copy, read seq again; use the copy only if both reads are the same
 * even number).
 *
 * Written by ft-hands (track/calibfile.cpp) when it starts and whenever it exchanges the side
 * cameras: it is the one that matches ring cameras to the factory calibration (XRService's log,
 * the side check), so cams[k].ring_cam is its current answer. ring_writer_pid is the ring's
 * writer_pid at the time: a file whose pid differs from the ring's describes an earlier ring.
 *
 * Frames: the head frame is OpenVR's HMD frame (+x right, +y up, -z forward), as in the hands
 * file. A camera's frame has +z along the optical axis, +x right and +y down in the image.
 * R's columns are the camera's axes in the head frame and t its centre there, in metres, so a
 * point c in the camera frame is at R c + t in the head frame. Intrinsics are for the ring's
 * images as they are (a colour camera's downscaled luma included), with pixel centres at
 * integer coordinates. The whole format: include/fh_calib.md.
 */

#pragma once

#include <assert.h>
#include <stdint.h>

#define FH_CALIB_MAGIC    "FHCALIB1"
#define FH_CALIB_VERSION  1
#define FH_CALIB_MAX_CAMS 8         /* FH_RING_MAX_CAMS */

enum {
    FH_CALIB_KB4 = 1,   /* Kannala-Brandt, OpenCV's fisheye model: a ray at angle theta from the
                           axis lands at radius theta (1 + k1 theta^2 + k2 theta^4 + k3 theta^6
                           + k4 theta^8) in normalized coordinates; k[0..3] are k1..k4          */
};

typedef struct {
    char     name[32];      /* the calibration's name: slam_left, slam_right, upper_left,
                               upper_right, or color_video<N> for an Arcturus colour camera     */
    int32_t  ring_cam;      /* index in the ring's cams[] whose images these are, -1 if none   */
    int32_t  node;          /* that ring camera's node, to check the match                      */
    uint32_t model;         /* FH_CALIB_*                                                       */
    uint32_t width;         /* the images this is for, pixels                                   */
    uint32_t height;
    uint32_t reserved0;
    double   fx, fy, cx, cy;
    double   k[8];          /* distortion, by model                                             */
    double   R[3][3];       /* row-major: R[i][j] is the camera's axis j, component i           */
    double   t[3];
    uint8_t  reserved[8];
} fh_calib_cam_t;           /* 256 bytes */

typedef struct {
    char              magic[8];
    uint32_t          version;
    uint32_t          size;
    volatile uint64_t seq;
    uint64_t          publish_ns;       /* CLOCK_MONOTONIC when this was written            */
    char              producer[16];     /* NUL-padded: "ft-hands"                           */
    int64_t           ring_writer_pid;  /* the ring's writer_pid these cameras are for      */
    uint32_t          ncams;
    uint32_t          reserved0;
    fh_calib_cam_t    cams[FH_CALIB_MAX_CAMS];
} fh_calib_t;

static_assert(sizeof(fh_calib_cam_t) == 256, "fh_calib_cam_t layout");
static_assert(sizeof(fh_calib_t) == 64 + 8 * 256, "fh_calib_t layout");
