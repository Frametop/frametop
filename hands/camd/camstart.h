/*
 * camstart: attach to XRService only once it has opened its tracking cameras.
 *
 * XRService opens the four mono tracking cameras one after another as it starts (the side pair,
 * then the upper pair a moment later, then their buffer queues) and again when it starts them
 * over. ft-camd used to take whatever was there the moment it found the process: started in that
 * window (the desktop autostarting with SteamVR at boot), it ran with two cameras until someone
 * turned hand tracking off and on ("only 2 of 4 tracking cameras running", DEE-392). It sees a
 * camera that goes silent, never one that appears.
 *
 * So the first look waits, a few seconds at most, until every tracking camera has a buffer queue.
 * A headset that really has fewer goes on with what it has after the wait.
 */
#ifndef FH_CAMSTART_H
#define FH_CAMSTART_H

#include "xrcams.h"

#include <stdbool.h>
#include <stddef.h>

#define CAMSTART_CAMERAS  4       /* a Frame's mono tracking cameras: the side pair, the upper pair */
#define CAMSTART_WAIT_S   10.0    /* XRService has them all within 1-2 s of starting (its log)      */
#define CAMSTART_STEP_MS  250

/* What the wait needs from the system: tests give their own. */
typedef struct {
    bool   (*discover)(xr_state_t *st, const char *process, char *err, size_t errn);
    double (*now)(void);            /* seconds, monotonic */
    void   (*sleep_ms)(int ms);
} camstart_io_t;

/* Mono cameras whose sensor name has `sensor` in it and that have a buffer queue to read
 * (xr_group_serves: the queues ft-camd would look for their frames in). */
int  camstart_ready(const xr_state_t *st, const char *sensor);

/* Whether to look again: not all of them yet, and the wait isn't over. */
bool camstart_waits(int ready, double waited_s);

/*
 * xr_discover, again until XRService has every tracking camera or CAMSTART_WAIT_S passed.
 * A --sensor choice (non-empty `sensor`) names its own cameras, so it doesn't wait. False,
 * with err, when XRService isn't running (or stops while this waits).
 */
bool camstart_attach(const camstart_io_t *io, xr_state_t *st, const char *process, const char *sensor,
                     char *err, size_t errn);

/* camstart_attach with the real XRService, clock and sleep. */
bool camstart_discover(xr_state_t *st, const char *process, const char *sensor, char *err, size_t errn);

#endif
