// ft-eyegrab --late's bookkeeping, offline: a frame copied when its camera starts the next one,
// and LATE_AFTER later, is compared row by row with the frame as it's passed on. A made-up
// 16 MiB buffer stands in for SteamVR's; no camera, SteamVR or root needed.
#define FT_EYEGRAB_NO_MAIN
#include "../ft-eyegrab.c"

#include <assert.h>

static uint8_t *row(int slot, int y) { return (uint8_t *)bufs[0].p + slot_start(slot) + (size_t)y * EYE_W; }

int main(void) {
    uint8_t *mem = calloc(1, 16777216);
    assert(mem);
    bufs[0].p = mem, bufs[0].size = 16777216, nbufs = 1;
    late.b = eye_buffer();
    assert(late.b == 0);
    for (int k = 0; k < EYE_SLOTS; k++) {
        late.taken[k] = -1;
        for (int i = 0; i < LATE_STEPS; i++) assert((late.copy[k][i] = malloc(EYE_W * EYE_H)));
    }
    for (int i = 0; i < LATE_STEPS; i++) assert((late.rows[i] = malloc(LATE_MAX * sizeof *late.rows[i])));

    // rows_differ: none, then two rows touched
    uint8_t a[EYE_W * EYE_H] = {0}, b[EYE_W * EYE_H] = {0};
    assert(rows_differ(a, b) == 0);
    b[5] = 1, b[399 * EYE_W + 511] = 1;
    assert(rows_differ(a, b) == 2);

    // A frame passed on before any copy was taken (it started before the probe): not counted
    late_done(bufs[0].p + slot_start(3), 3, 0.0);
    assert(late.whole == 0);

    // Slot 3 finished at t=1.0; 10 rows land 2 ms later, 4 more 5 ms later, then the frame after next
    late_started(3, 1.0);
    late_tick(1.001);
    assert(late.taken[3] == 1);  // the 3 ms copy isn't due yet
    for (int y = 390; y < 400; y++) row(3, y)[0] = 7;
    late_tick(1.0031);
    assert(late.taken[3] == 2);
    for (int y = 386; y < 390; y++) row(3, y)[0] = 7;
    late_tick(1.0062);
    assert(late.taken[3] == 3);
    late_tick(1.009);  // nothing more to take
    assert(late.taken[3] == 3);
    late_done(bufs[0].p + slot_start(3), 3, 1.0);
    assert(late.whole == 1 && late.taken[3] == -1);
    assert(late.n[0] == 1 && late.rows[0][0] == 14);  // at the next frame's start: 14 rows still to come
    assert(late.n[1] == 1 && late.rows[1][0] == 4);   // 3 ms later: 4
    assert(late.n[2] == 1 && late.rows[2][0] == 0);   // 6 ms later: whole

    // A clean frame in the other camera's slot: no rows at any copy
    late_started(5, 2.0);
    late_tick(2.01);
    late_done(bufs[0].p + slot_start(5), 5, 2.0);
    assert(late.whole == 2 && late.rows[0][1] == 0 && late.rows[1][1] == 0 && late.rows[2][1] == 0);

    printf("eyegrab-late-test: ok\n");
    return 0;
}
