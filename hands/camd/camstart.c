/* camstart: attach to XRService only once it has opened its tracking cameras (camstart.h). */
#include "camstart.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int camstart_ready(const xr_state_t *st, const char *sensor)
{
    int ready = 0;

    for (int i = 0; i < st->ncameras; i++) {

        const xr_camera_t *cam = &st->cameras[i];
        xr_layout_t lay;

        xr_camera_layout(cam, &lay);

        if (lay.fmt != XR_FMT_GREY8 || !strstr(cam->sensor, sensor))
            continue;

        size_t need = (size_t)lay.pitch * lay.rows;

        for (int g = 0; g < st->ngroups; g++)
            if (xr_group_serves(st, &st->groups[g], cam, need)) {
                ready++;
                break;
            }
    }

    return ready;
}

bool camstart_waits(int ready, double waited_s)
{
    return ready < CAMSTART_CAMERAS && waited_s < CAMSTART_WAIT_S;
}

bool camstart_attach(const camstart_io_t *io, xr_state_t *st, const char *process, const char *sensor,
                     char *err, size_t errn)
{
    double start = io->now();
    bool waited = false;

    for (;;) {

        if (!io->discover(st, process, err, errn))
            return false;

        int ready = camstart_ready(st, sensor);
        double since = io->now() - start;

        if (sensor[0] || !camstart_waits(ready, since)) {
            if (waited && ready >= CAMSTART_CAMERAS)
                fprintf(stderr, "%s has all %d tracking cameras after %.1f s\n", process, ready, since);
            else if (waited)
                fprintf(stderr, "%s still has only %d of %d tracking cameras after %.0f s: going on with those\n",
                        process, ready, CAMSTART_CAMERAS, since);
            return true;
        }

        if (!waited)
            fprintf(stderr, "%s has only %d of %d tracking cameras ready (it is still starting them): "
                    "waiting up to %.0f s for the rest\n", process, ready, CAMSTART_CAMERAS, CAMSTART_WAIT_S);
        waited = true;
        io->sleep_ms(CAMSTART_STEP_MS);
    }
}

static double real_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void real_sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

bool camstart_discover(xr_state_t *st, const char *process, const char *sensor, char *err, size_t errn)
{
    static const camstart_io_t real = { xr_discover, real_now, real_sleep_ms };

    return camstart_attach(&real, st, process, sensor, err, errn);
}
