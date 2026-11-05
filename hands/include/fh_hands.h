/*
 * fh_hands - the tracked-hands file ft-hands publishes for ft-screens' hand cutouts
 * (/run/user/UID/frametop-hands/hands, directory mode 0700), rewritten in place
 * under a sequence lock: read seq, copy, read seq again; use the copy only if
 * both reads are the same even number.
 *
 * Positions are metres in the head frame at capture time, which is OpenVR's HMD
 * frame (+x right, +y up, -z forward). Turn them into the room with the HMD pose
 * at capture_ns (CLOCK_MONOTONIC). Writer: ft-hands (hands/track/io.cpp).
 *
 * Version 2 appends a skeleton per hand (fh_hand_fit_t) after version 1's fh_hands_t, which is
 * unchanged: a version 1 reader that maps sizeof(fh_hands_t) and checks the magic keeps
 * working. Readers of the skeleton check version >= 2 and the file's size >= sizeof(fh_hands_v2_t).
 */

#pragma once

#include <assert.h>
#include <stdint.h>

#define FH_HANDS_MAGIC        "FHHANDS1"
#define FH_HANDS_VERSION      2
#define FH_HANDS_MAX_HANDS    2
#define FH_HANDS_MAX_CAPSULES 64

enum {
    FH_HAND_RIGHT  = 1u << 0,   /* else the left hand                          */
    FH_HAND_STEREO = 1u << 1,   /* triangulated from two or more cameras       */
};

typedef struct {
    uint32_t id;                /* stays the same while the hand is tracked     */
    uint32_t flags;             /* FH_HAND_*                                    */
    float    confidence;
    float    reserved;
    float    pts[21][3];        /* MediaPipe hand landmarks                     */
    uint32_t ncapsules;         /* this hand's capsules, which follow the       */
                                /* previous hands' in capsules[]                */
} fh_hand_t;                    /* 272 bytes */

typedef struct {
    float a[3], b[3];           /* segment ends                                 */
    float ra, rb;               /* radius at each end                           */
} fh_capsule_t;                 /* 32 bytes: the hand's shape, to cut out       */

typedef struct {
    char              magic[8];
    uint32_t          version;
    uint32_t          size;
    volatile uint64_t seq;
    uint64_t          capture_ns;   /* CLOCK_MONOTONIC when the cameras took the frames */
    uint64_t          publish_ns;   /* CLOCK_MONOTONIC when this was written            */
    uint32_t          nhands;
    uint32_t          ncapsules;
    uint8_t           reserved[16];
    fh_hand_t         hands[FH_HANDS_MAX_HANDS];
    fh_capsule_t      capsules[FH_HANDS_MAX_CAPSULES];
} fh_hands_t;

/* ---- version 2: the fitted skeleton (hands/track/skeleton.h, hands/README.md "Skeleton") ----
 *
 * Bone lengths are learned per user and side, so fitted fingers keep their length; the pose
 * (palm and joint angles) is filtered, not the points. Everything is in the head frame at
 * capture time, like pts. Joint frames follow OpenXR's hand joints: -Z along the bone that
 * starts at the joint (toward the fingertip), +Y out of the back of the hand, +X = Y x Z
 * (so +X points to the little finger on a right hand, to the thumb on a left one). The
 * wrist's frame is the palm's: -Z toward the middle knuckle, +Y the back of the hand's normal.
 * A tip has its bone's frame. Joints are indexed like MediaPipe's 21 landmarks. */

enum {
    FH_FIT_VALID   = 1u << 0,   /* this hand has a skeleton                                   */
    FH_FIT_RIGHT   = 1u << 1,   /* fitted as a right hand (else left)                          */
    FH_FIT_SETTLED = 1u << 2,   /* its side's bone lengths have settled (else partly the prior) */
    FH_FIT_FLIPPED = 1u << 3,   /* fitted as the other side than the model called it           */
    FH_FIT_ELBOW_HEAD_UP = 1u << 4,   /* elbow from the arm model with the head's +y as up     */
};

typedef struct {
    float p[3];                 /* position, m                                                 */
    float radius;               /* rough, m: for colliders and a mesh's thickness              */
    float q[4];                 /* orientation x, y, z, w: the joint's frame (above)           */
} fh_joint_t;                   /* 32 bytes */

typedef struct {
    uint32_t   id;              /* hands[k].id; this is hands[k]'s skeleton                    */
    uint32_t   flags;           /* FH_FIT_*                                                    */
    uint64_t   tracked_ns;      /* CLOCK_MONOTONIC capture time of the last frame the hand was
                                   measured in (today always capture_ns)                       */
    fh_joint_t joints[21];
    float      bone_len[20];    /* m; bone b ends at joint b + 1 (bones 0, 4, 8, 12, 16 start
                                   at the wrist)                                               */
    float      angles[26];      /* rad: thumb CMC abduction, flexion, twist, MCP abduction,
                                   flexion, IP flexion; then per finger (index, middle, ring,
                                   little) MCP abduction, MCP flexion, MCP twist, PIP, DIP.
                                   Flexion is positive toward the palm, abduction toward the
                                   thumb's side                                                */
    float      elbow[3];        /* the arm model's elbow (notes: frame-hands forearm.md)       */
    float      elbow_conf;      /* 0: inferred by the arm model only (as now)                  */
    float      pinch_point[3];  /* between the index and middle knuckles: the gestures file's
                                   pinch point, from the skeleton                              */
    float      contact[3];      /* midway between the thumb and index tips                     */
    float      pinch_d[2];      /* thumb tip to index tip, thumb tip to middle tip, m          */
    float      fit_rms;         /* the fit against this frame's measured landmarks, m          */
    float      settled;         /* 0..1: how far its side's bone-length calibration is         */
    float      vel_head[3];     /* palm centre velocity in the HEAD frame, m/s, filtered. It
                                   includes the head's own motion: turning the head reads as
                                   hand motion. For motion in the room, difference room-frame
                                   points across publishes (capture_ns) as handcut.cpp does.  */
    float      angvel_head[3];  /* the palm's angular velocity in the head frame, rad/s; it
                                   includes the head's rotation, as vel_head                   */
    float      reserved[18];
} fh_hand_fit_t;                /* 1024 bytes */

typedef struct {
    fh_hands_t    v1;           /* version 1, unchanged; v1.version is 2                       */
    fh_hand_fit_t fits[FH_HANDS_MAX_HANDS];   /* fits[k] goes with v1.hands[k]; zero when
                                                 k >= nhands or the hand has no skeleton       */
} fh_hands_v2_t;

static_assert(sizeof(fh_hand_t) == 272, "fh_hand_t layout");
static_assert(sizeof(fh_joint_t) == 32, "fh_joint_t layout");
static_assert(sizeof(fh_hand_fit_t) == 1024, "fh_hand_fit_t layout");
static_assert(sizeof(fh_capsule_t) == 32, "fh_capsule_t layout");
static_assert(sizeof(fh_hands_t) == 64 + 2 * 272 + 64 * 32, "fh_hands_t layout");
static_assert(sizeof(fh_hands_v2_t) == sizeof(fh_hands_t) + 2 * 1024, "fh_hands_v2_t layout");
