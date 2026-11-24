/*
 * hands/camd/camstart.c, offline: ft-camd's first look at XRService waits until every tracking
 * camera has a buffer queue (DEE-392: started while XRService was still opening them, it ran
 * with two of four). A made-up XRService that opens its cameras over time, a made-up clock;
 * no device, nothing opened.
 *
 *   gcc -std=gnu11 -Wall -Wextra -Werror hands/tests/camstart_test.c -o build/tests/camstart-test
 */
#include "../camd/xrcams.c"
#include "../camd/camstart.c"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* The made-up XRService: how many cameras and queues it has by each look, and the clock. */
static int    looks, slept_ms, gone_at_look = -1;
static int    cameras_at[64], queues_at[64];
static double clock_s;

static void add_camera(xr_state_t *st, unsigned w, unsigned h, uint32_t fmt, const char *sensor)
{
    xr_camera_t *c = &st->cameras[st->ncameras++];

    c->width = w, c->height = h, c->pixfmt = fmt, c->bytesperline = w;
    c->planesize[0] = (size_t)w * h;
    snprintf(c->sensor, sizeof c->sensor, "%s", sensor);
    c->role = "tracking";
}

static void add_queue(xr_state_t *st, size_t plane0, const char *sensor)
{
    xr_group_t *g = &st->groups[st->ngroups++];

    g->nbufs = 16;
    g->planesize[0] = plane0;
    snprintf(g->sensor, sizeof g->sensor, "%s", sensor);
}

/* The Frame with its colour module: side pair 1056x1024, upper pair 640x480, all grey. */
static bool fake_discover(xr_state_t *st, const char *process, char *err, size_t errn)
{
    static const struct { unsigned w, h; const char *sensor; } CAMS[4] = {
        { 1056, 1024, "og01a1b 4-0036" }, { 1056, 1024, "og01a1b 4-0060" },
        { 640, 480, "og0ve10 5-0060" }, { 640, 480, "og0ve10 5-0061" } };
    int look = looks < 63 ? looks : 63;

    looks++;
    memset(st, 0, sizeof *st);

    if (gone_at_look >= 0 && look >= gone_at_look) {
        snprintf(err, errn, "%s is not running", process);
        return false;
    }

    for (int i = 0; i < cameras_at[look]; i++)
        add_camera(st, CAMS[i].w, CAMS[i].h, V4L2_PIX_FMT_GREY, CAMS[i].sensor);
    for (int i = 0; i < queues_at[look]; i++)
        add_queue(st, (size_t)CAMS[i].w * CAMS[i].h, CAMS[i].sensor);

    return true;
}

static double fake_now(void) { return clock_s; }
static void fake_sleep(int ms) { slept_ms += ms; clock_s += ms / 1000.0; }

static const camstart_io_t FAKE = { fake_discover, fake_now, fake_sleep };

/* An XRService with `cams` cameras and `queues` queues from look `from` on. */
static void from_look(int from, int cams, int queues)
{
    for (int i = from; i < 64; i++)
        cameras_at[i] = cams, queues_at[i] = queues;
}

static void reset(void)
{
    looks = slept_ms = 0;
    gone_at_look = -1;
    clock_s = 100.0;
    from_look(0, 0, 0);
}

static xr_state_t st;

int main(void)
{
    char err[128];

    CHECK(camstart_waits(2, 0.0) && camstart_waits(3, 9.9) && camstart_waits(0, 0.0), "fewer, in time: wait");
    CHECK(!camstart_waits(4, 0.0) && !camstart_waits(5, 0.0), "all of them: go");
    CHECK(!camstart_waits(2, CAMSTART_WAIT_S), "the wait is over: go with what's there");

    /* all four there at once (the desktop started long after SteamVR): one look, no wait */
    reset();
    from_look(0, 4, 4);
    CHECK(camstart_attach(&FAKE, &st, "XRService", "", err, sizeof err), "attach");
    CHECK(looks == 1 && slept_ms == 0 && camstart_ready(&st, "") == 4, "looks %d slept %d", looks, slept_ms);

    /* DEE-392: the side pair first, the upper pair two looks later */
    reset();
    from_look(0, 2, 2);
    from_look(2, 4, 4);
    CHECK(camstart_attach(&FAKE, &st, "XRService", "", err, sizeof err), "attach");
    CHECK(looks == 3 && slept_ms == 2 * CAMSTART_STEP_MS, "looks %d slept %d", looks, slept_ms);
    CHECK(st.ncameras == 4 && camstart_ready(&st, "") == 4, "it has all four: %d", camstart_ready(&st, ""));

    /* a camera that's open but has no buffer queue yet isn't ready: an upper camera's image would
     * fit a side camera's bigger queue, but that queue isn't one ft-camd reads it from */
    reset();
    from_look(0, 4, 2);
    from_look(1, 4, 4);
    CHECK(camstart_attach(&FAKE, &st, "XRService", "", err, sizeof err) && looks == 2, "looks %d", looks);
    st.ngroups = 2;
    CHECK(camstart_ready(&st, "") == 2, "queues for the side pair only: %d ready", camstart_ready(&st, ""));

    /* a headset that keeps having two: it goes on with them once the wait is over, not before */
    reset();
    from_look(0, 2, 2);
    CHECK(camstart_attach(&FAKE, &st, "XRService", "", err, sizeof err), "attach");
    CHECK(slept_ms >= (int)(CAMSTART_WAIT_S * 1000) && slept_ms < (int)(CAMSTART_WAIT_S * 1000) + 2 * CAMSTART_STEP_MS,
          "slept %d ms", slept_ms);
    CHECK(st.ncameras == 2, "it has the two");

    /* XRService stops while it waits: false, with its word, so the unit starts ft-camd again */
    reset();
    from_look(0, 2, 2);
    gone_at_look = 3;
    err[0] = 0;
    CHECK(!camstart_attach(&FAKE, &st, "XRService", "", err, sizeof err) && looks == 4 && strstr(err, "not running"),
          "looks %d err '%s'", looks, err);

    /* --sensor names its own cameras: no wait for the others */
    reset();
    from_look(0, 2, 2);
    CHECK(camstart_attach(&FAKE, &st, "XRService", "og01a1b", err, sizeof err) && looks == 1 && slept_ms == 0,
          "looks %d slept %d", looks, slept_ms);
    CHECK(camstart_ready(&st, "og01a1b") == 2 && camstart_ready(&st, "og0ve10") == 0, "by sensor name");

    printf("camstart_test: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures != 0;
}
