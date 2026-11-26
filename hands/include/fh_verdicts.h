/*
 * fh_verdicts - what the pointer helper did with each pinch ft-hands published, so ft-hands
 * learns its wearer's pinch only from pinches that became a click, a press, a drag or a scroll
 * (track/profile.h, PinchObserver). ft-hands can't tell a tap from a near miss or a typing hand
 * on its own; the helper's rules (typing, a controller in the hand, a fast stroke, a closing
 * hand...) can, and they've decided by the time the pinch is over.
 *
 * /run/user/UID/frametop-hands/verdicts, next to the gestures file, rewritten in place under
 * the same sequence lock (read seq, copy, read seq again; use the copy only if both reads are the
 * same even number). One writer, the pointer helper; ft-hands reads it with fh_shm.hpp. The last
 * FH_VERDICTS_RING verdicts are kept in a ring; count says how many were ever written, so a
 * reader sees which are new (count - FH_VERDICTS_RING of them at most are still there).
 *
 * A verdict: the gesture (side, kind: 0 the pinch, 2 the middle pinch, as fh_gestures.h's slots)
 * by its begin_ns (fh_pinch_t.begin_ns, the capture time it began), and its outcome: done (it
 * clicked, pressed, dragged or scrolled), refused (a rule held it back), passed (left to
 * ft-screens: a hand at a panel), lost (it ended because the hand was lost). Nothing is written
 * for a pinch the helper never saw.
 */
#ifndef FH_VERDICTS_H
#define FH_VERDICTS_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FH_VERDICTS_FILE    "verdicts"
#define FH_VERDICTS_MAGIC   "FHVERD01"
#define FH_VERDICTS_VERSION 1
#define FH_VERDICTS_RING    32

enum { FH_VERDICT_DONE = 1, FH_VERDICT_REFUSED = 2, FH_VERDICT_PASSED = 3, FH_VERDICT_LOST = 4 };

typedef struct {
    uint64_t begin_ns;   /* the gesture's fh_pinch_t.begin_ns                  */
    uint64_t t_ns;       /* CLOCK_MONOTONIC when the helper decided             */
    uint8_t  side;       /* 0 left, 1 right                                    */
    uint8_t  kind;       /* 0 pinch, 2 middle pinch                            */
    uint8_t  outcome;    /* FH_VERDICT_*                                        */
    uint8_t  reserved[5];
} fh_verdict_t;          /* 24 bytes */

typedef struct {
    char              magic[8];
    uint32_t          version;
    uint32_t          size;
    volatile uint64_t seq;
    uint64_t          count;   /* verdicts written so far; the newest is ring[(count - 1) % RING] */
    fh_verdict_t      ring[FH_VERDICTS_RING];
} fh_verdicts_t;

#ifdef __cplusplus
static_assert(sizeof(fh_verdict_t) == 24, "fh_verdict_t layout");
static_assert(sizeof(fh_verdicts_t) == 32 + FH_VERDICTS_RING * 24, "fh_verdicts_t layout");
#endif

/* The writer's mapping of DIR/verdicts, made (0600) and set up if it isn't, or NULL. A failed
 * try isn't repeated within a second, so a missing folder costs nothing per pinch. */
static inline fh_verdicts_t *fh_verdicts_map(const char *dir) {
    static fh_verdicts_t *map = NULL;
    static char mapped_dir[128];
    static time_t tried = 0;
    if (map && strcmp(mapped_dir, dir) == 0) return map;
    if (map) munmap(map, sizeof *map), map = NULL;
    const time_t now = time(NULL);
    if (tried && now - tried < 1) return NULL;
    tried = now;
    char path[192];
    snprintf(path, sizeof path, "%s/" FH_VERDICTS_FILE, dir);
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    void *m = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_uid == getuid() &&
        (st.st_size == (off_t)sizeof(fh_verdicts_t) || ftruncate(fd, sizeof(fh_verdicts_t)) == 0))
        m = mmap(NULL, sizeof(fh_verdicts_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;
    map = (fh_verdicts_t *)m;
    snprintf(mapped_dir, sizeof mapped_dir, "%s", dir);
    if (memcmp(map->magic, FH_VERDICTS_MAGIC, 8) != 0 || map->version != FH_VERDICTS_VERSION) {
        memset(map, 0, sizeof *map);
        map->version = FH_VERDICTS_VERSION;
        map->size = sizeof *map;
        memcpy(map->magic, FH_VERDICTS_MAGIC, 8);   /* last: a reader checks it first */
    }
    return map;
}

/* One verdict into DIR/verdicts (the runtime folder, /run/user/UID/frametop-hands, if dir is NULL). */
static inline void fh_verdict(const char *dir, int side, int kind, int outcome, uint64_t begin_ns) {
    char runtime[64];
    if (!dir) snprintf(runtime, sizeof runtime, "/run/user/%u/frametop-hands", (unsigned)getuid()), dir = runtime;
    if (side < 0 || side > 1 || !begin_ns) return;
    fh_verdicts_t *v = fh_verdicts_map(dir);
    if (!v) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fh_verdict_t r;
    memset(&r, 0, sizeof r);
    r.begin_ns = begin_ns;
    r.t_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    r.side = (uint8_t)side, r.kind = (uint8_t)kind, r.outcome = (uint8_t)outcome;
    __atomic_store_n(&v->seq, v->seq + 1, __ATOMIC_RELEASE);   /* odd: being written */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    v->ring[v->count % FH_VERDICTS_RING] = r;
    v->count = v->count + 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&v->seq, v->seq + 1, __ATOMIC_RELEASE);   /* even: done */
}

#endif
