/* Reads the hands and gestures files through hands/include's structs and prints what
 * test_fakes.py checks, so the fake trackers' layouts are tested against the real ones.
 * Usage: fake_hands_reader DIR (the folder with hands and gestures). */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fh_gestures.h"
#include "fh_hands.h"

static const void *map(const char *dir, const char *name, size_t size) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static void vec(const char *name, const float *v, int n) {
    printf("\"%s\":[", name);
    for (int i = 0; i < n; ++i) printf("%s%.9g", i ? "," : "", v[i]);
    printf("],");
}

static void pinch(const char *name, const fh_pinch_t *g) {
    printf("\"%s\":{\"flags\":%u,\"hand_id\":%u,\"begins\":%u,\"ends\":%u,\"distance\":%.9g,\"strength\":%.9g,",
           name, g->flags, g->hand_id, g->begins, g->ends, g->distance, g->strength);
    vec("point", g->point, 3);
    printf("\"begin_ns\":%llu},", (unsigned long long)g->begin_ns);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const fh_hands_v2_t *h = map(argv[1], "hands", sizeof *h);
    const fh_gestures_t *g = map(argv[1], "gestures", sizeof *g);
    if (!h || !g) return 1;
    printf("{\"hands\":{\"magic_ok\":%d,\"version\":%u,\"size\":%u,\"seq\":%llu,\"nhands\":%u,\"ncapsules\":%u,",
           memcmp(h->v1.magic, FH_HANDS_MAGIC, 8) == 0, h->v1.version, h->v1.size,
           (unsigned long long)h->v1.seq, h->v1.nhands, h->v1.ncapsules);
    printf("\"capture_ns\":%llu,\"list\":[", (unsigned long long)h->v1.capture_ns);
    for (unsigned k = 0; k < h->v1.nhands && k < FH_HANDS_MAX_HANDS; ++k) {
        const fh_hand_t *hand = &h->v1.hands[k];
        const fh_hand_fit_t *fit = &h->fits[k];
        printf("%s{\"id\":%u,\"flags\":%u,\"confidence\":%.9g,\"ncapsules\":%u,", k ? "," : "", hand->id,
               hand->flags, hand->confidence, hand->ncapsules);
        vec("pts", &hand->pts[0][0], 63);
        printf("\"fit\":{\"id\":%u,\"flags\":%u,\"tracked_ns\":%llu,", fit->id, fit->flags,
               (unsigned long long)fit->tracked_ns);
        vec("joint8_p", fit->joints[8].p, 3);
        vec("joint8_q", fit->joints[8].q, 4);
        printf("\"joint8_radius\":%.9g,", fit->joints[8].radius);
        vec("bone_len", fit->bone_len, 20);
        vec("elbow", fit->elbow, 3);
        vec("pinch_point", fit->pinch_point, 3);
        vec("contact", fit->contact, 3);
        vec("pinch_d", fit->pinch_d, 2);
        printf("\"settled\":%.9g}}", fit->settled);
    }
    printf("],");
    vec("capsule0", h->v1.capsules[0].a, 8);
    printf("\"end\":0},\"gestures\":{\"magic_ok\":%d,\"version\":%u,\"size\":%u,\"seq\":%llu,",
           memcmp(g->magic, FH_GESTURES_MAGIC, 8) == 0, g->version, g->size, (unsigned long long)g->seq);
    printf("\"begin_m\":%.9g,\"end_m\":%.9g,\"grip_begin\":%.9g,\"grip_end\":%.9g,", g->begin_m, g->end_m,
           g->grip_begin, g->grip_end);
    pinch("pinch_left", &g->pinch[0]);
    pinch("pinch_right", &g->pinch[1]);
    pinch("grip_left", &g->grip[0]);
    pinch("grip_right", &g->grip[1]);
    pinch("pinch2_left", &g->pinch2[0]);
    pinch("pinch2_right", &g->pinch2[1]);
    printf("\"end\":0}}\n");
    return 0;
}
