/*
 * fh_gestures - hand gestures ft-hands publishes for input (the pointer helper): look at
 * something and pinch to click it, or close the hand (a grip) to press and drag it (the
 * Vision Pro model, with the eye tracker doing the looking).
 * /run/user/UID/frametop-hands/gestures, next to the hands
 * file, with the same sequence lock (read seq, copy, read seq again; use the copy only if
 * both reads are the same even number) and the same frame: metres in the head frame at
 * capture time, OpenVR's HMD frame (+x right, +y up, -z forward).
 *
 * One slot per side and gesture: pinch[0] and grip[0] are the left hand, [1] the right.
 * A gesture follows the hand it began on until it ends.
 *   Pinch: begins when the thumb and index tips close within begin_m and ends when they
 * open past end_m (the gap between keeps it from flickering). point is the index and middle
 * knuckles, which don't move as the fingers open and close (the tips' midpoint did).
 *   Grip: a closed hand. It begins when all four fingers are curled in (each fingertip
 * nearer the wrist than grip_begin times its knuckle is) and ends when they open past
 * grip_end on average. distance is that average (about 2 open, under 1.2 closed), strength
 * 0 open .. 1 closed, and point the palm's centre. A grip ends a pinch on the same hand
 * (closing the hand can pass through a pinch on the way), as lost.
 * Either ends, as lost (FH_PINCH_LOST), when its hand stays lost too long.
 *
 * Don't miss short gestures: a reader that polls slower than a quick tap still sees it,
 * because begins and ends count every one. When begins changed, one began at begin_ns;
 * when ends changed, one ended at end_ns. begins - ends is 1 while it's down.
 *
 * Drags: point is where the gesture is now, begin_point where it began. Turn each into the
 * room with the HMD pose at its capture time (capture_ns, begin_ns) before subtracting,
 * so turning your head doesn't drag.
 *
 *   Middle pinch (version 3): the thumb on the middle fingertip instead of the index one, with
 * the pinch's thresholds and point. distance is thumb tip to middle tip. It begins only while
 * the thumb is clearly nearer the middle tip than the index tip, and an index pinch and a middle
 * pinch on one hand exclude each other (whichever began first holds until it ends). The pointer
 * helper's hand ray turns it into a right click (a tap) or scrolling (held and moved).
 *
 * Version 1 had only the pinches (192 bytes); version 2 adds the grips after them, version 3
 * the middle pinches after those. A reader of an older version maps and copies the size it
 * knows: the file only grows at the end.
 */

#pragma once

#include <assert.h>
#include <stdint.h>

#define FH_GESTURES_MAGIC   "FHGEST01"
#define FH_GESTURES_VERSION 3

enum {
    FH_PINCH_TRACKED = 1u << 0, /* the hand was tracked in this frame              */
    FH_PINCH_DOWN    = 1u << 1, /* the gesture is held now                          */
    FH_PINCH_LOST    = 1u << 2, /* the last one ended because the hand was lost     */
                                /* (or, for a pinch, a grip took over)              */
};

typedef struct {
    uint32_t flags;             /* FH_PINCH_*                                       */
    uint32_t hand_id;           /* fh_hand_t.id of the hand, 0 if none              */
    uint32_t begins;            /* begun so far                                     */
    uint32_t ends;              /* ended so far                                     */
    uint64_t begin_ns;          /* capture time (CLOCK_MONOTONIC) the current or    */
                                /* last one began                                   */
    uint64_t end_ns;            /* ... the last one ended                           */
    float    distance;          /* pinch: thumb tip to index tip, m, at this user's */
                                /* hand size. grip: the fingers' mean curl (above)  */
    float    strength;          /* 0 open .. 1 closed                               */
    float    point[3];          /* pinch: the index and middle knuckles; grip: the  */
                                /* palm's centre                                    */
    float    begin_point[3];    /* point when the current or last one began         */
} fh_pinch_t;                   /* 64 bytes */

typedef struct {
    char              magic[8];
    uint32_t          version;
    uint32_t          size;
    volatile uint64_t seq;
    uint64_t          capture_ns;   /* CLOCK_MONOTONIC when the cameras took the frames */
    uint64_t          publish_ns;   /* CLOCK_MONOTONIC when this was written            */
    float             begin_m;      /* the pinch thresholds in use                      */
    float             end_m;
    float             grip_begin;   /* the grip thresholds in use (curl ratios)         */
    float             grip_end;
    uint8_t           reserved[8];
    fh_pinch_t        pinch[2];     /* [0] left hand, [1] right hand                    */
    fh_pinch_t        grip[2];      /* version 2                                        */
    fh_pinch_t        pinch2[2];    /* version 3: thumb to middle fingertip             */
} fh_gestures_t;

static_assert(sizeof(fh_pinch_t) == 64, "fh_pinch_t layout");
static_assert(sizeof(fh_gestures_t) == 64 + 6 * 64, "fh_gestures_t layout");
