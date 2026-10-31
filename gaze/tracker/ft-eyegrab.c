/*
 * ft-eyegrab: the eye-camera frames for our own eye tracker (ft-eyes), copied read-only out
 * of the DMA-BUFs SteamVR's eyetracking process holds. Runs as root (pidfd_getfd needs
 * CAP_SYS_PTRACE; ptrace_scope is 1): the system service frametop-eyegrab.service runs
 * --share, installed by gaze/tracker/install.sh. The other modes are for finding the frames
 * again after a SteamVR update (run them with sudo).
 *
 *   ft-eyegrab --share PATH [--owner UID:GID] [--want FILE]
 *                              keep the latest frames of both cameras in PATH (shared memory,
 *                              0600, owned by UID:GID, or the sudo user) for ft-eyes; follows
 *                              the tracker through SteamVR restarts. With --want, only while
 *                              FILE (a regular file owned by that user) was touched in the
 *                              last WANT_FRESH seconds: ft-eyes and ft-eyes-record touch it
 *                              every second, so nothing is copied, and none of the tracker's
 *                              buffers are held, while nobody reads the frames
 *   ft-eyegrab                 list the buffers
 *   ft-eyegrab --scan [N]      N snapshots (default 40) about 11 ms apart: which 4 KiB pages
 *                              change, merged into regions, with byte statistics for each
 *   ft-eyegrab --dump I OFF LEN FILE
 *                              copy LEN bytes at OFF of buffer I (from the list) to FILE
 *   ft-eyegrab --seq I OFF LEN FRAMES DIR
 *                              FRAMES copies of that region, one each time it changes, to
 *                              DIR/NNNN.raw, with DIR/times.txt (CLOCK_MONOTONIC_RAW)
 *   ft-eyegrab --rec SECONDS DIR
 *                              every new eye-camera frame for SECONDS: DIR/frames.raw (512x400
 *                              8-bit frames back to back) and DIR/index.txt, one line per frame:
 *                              "<n> <slot> <camera 0|1> <CLOCK_MONOTONIC_RAW time seen>"
 *                              (lab/ft-eyes-record does the same from the shared frames,
 *                              without root)
 *
 * The eye frames (found with --scan): in the 16 MiB buffer, eight slots 0x40000 apart from
 * 0x230000, four per camera (slots 0-3, 4-7). Each slot starts with a small block, then a
 * 512x400 8-bit image at 0x40c0 + 0x40 per slot, and one more 0x40 for the second camera's.
 *
 * Only reads the tracker's buffers. They are borrowed with pidfd_getfd and mapped PROT_READ; nothing is
 * written, and the process isn't stopped or signalled. Reads can tear while the DSP writes.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAXBUF 32
#define PAGE 4096

typedef struct {
    int xfd, fd;
    size_t size;
    unsigned long ino;
    const uint8_t *p;
} buf_t;

static buf_t bufs[MAXBUF];
static int nbufs;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// A lab mode's output file (--rec, --seq, --dump): eye images, so only its owner may read it.
// The modes run with sudo, so the file goes to the sudo user, whose lab tools read it. A link
// isn't followed and a file with other names (a hard link) isn't written: root writes where
// the user said, and that must not reach another file. NULL with errno set when it can't.
static FILE *create_private(const char *path, const char *how) {
    int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        close(fd);
        errno = EPERM;
        return NULL;
    }
    const char *uid = getenv("SUDO_UID"), *gid = getenv("SUDO_GID");
    if (geteuid() == 0 && uid && gid && fchown(fd, (uid_t)atoi(uid), (gid_t)atoi(gid)) != 0) perror("fchown");
    if (fchmod(fd, 0600) != 0 || ftruncate(fd, 0) != 0) {
        close(fd);
        return NULL;
    }
    FILE *f = fdopen(fd, how);
    if (!f) close(fd);
    return f;
}

static int find_tracker(void) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int pid = -1;
    while (d && (e = readdir(d))) {
        char path[300], cmd[512];
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(cmd, 1, sizeof cmd - 1, f);
        fclose(f);
        cmd[n] = 0;
        if (strstr(cmd, "tools/eyetracking/bin/") && strstr(cmd, "/eyetracking")) {
            pid = atoi(e->d_name);
            break;
        }
    }
    if (d) closedir(d);
    return pid;
}

static int open_bufs(int pid) {
    int pidfd = syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        perror("pidfd_open");
        return -1;
    }
    char dir[64];
    snprintf(dir, sizeof dir, "/proc/%d/fd", pid);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && nbufs < MAXBUF) {
        char link[320], target[256];
        if (e->d_name[0] == '.') continue;
        snprintf(link, sizeof link, "%s/%s", dir, e->d_name);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (strncmp(target, "/dmabuf:", 8) != 0) continue;
        int xfd = atoi(e->d_name);
        int fd = syscall(SYS_pidfd_getfd, pidfd, xfd, 0);
        if (fd < 0) {
            fprintf(stderr, "pidfd_getfd %d: %s\n", xfd, strerror(errno));
            continue;
        }
        struct stat st = {0};
        if (fstat(fd, &st) != 0) perror("fstat");
        off_t size = lseek(fd, 0, SEEK_END);
        void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "mmap fd %d (%lld bytes): %s\n", xfd, (long long)size, strerror(errno));
            close(fd);
            continue;
        }
        bufs[nbufs++] = (buf_t){xfd, fd, (size_t)size, (unsigned long)st.st_ino, p};
    }
    if (d) closedir(d);
    close(pidfd);
    return nbufs;
}

static uint64_t page_hash(const uint8_t *p) {
    const uint64_t *q = (const uint64_t *)p;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < PAGE / 8; i += 4) h = (h ^ q[i]) * 1099511628211ull;  // every 4th word
    return h;
}

static void stats(const uint8_t *p, size_t n, double *mean, int *lo, int *hi, double *nonzero) {
    uint64_t sum = 0, nz = 0;
    int a = 255, b = 0;
    for (size_t i = 0; i < n; i++) {
        sum += p[i];
        nz += p[i] != 0;
        if (p[i] < a) a = p[i];
        if (p[i] > b) b = p[i];
    }
    *mean = n ? (double)sum / n : 0;
    *lo = a, *hi = b, *nonzero = n ? (double)nz / n : 0;
}

static void scan(int snaps) {
    for (int b = 0; b < nbufs; b++) {
        size_t pages = bufs[b].size / PAGE;
        uint64_t *prev = calloc(pages, 8), *cur = calloc(pages, 8);
        int *changes = calloc(pages, sizeof(int));
        for (size_t i = 0; i < pages; i++) prev[i] = page_hash(bufs[b].p + i * PAGE);
        double t0 = now();
        for (int s = 1; s < snaps; s++) {
            usleep(11000);
            for (size_t i = 0; i < pages; i++) {
                cur[i] = page_hash(bufs[b].p + i * PAGE);
                if (cur[i] != prev[i]) changes[i]++;
                prev[i] = cur[i];
            }
        }
        double dt = now() - t0;
        printf("buffer %d (fd %d, %zu bytes, ino %lu): %d snapshots over %.2f s\n", b, bufs[b].xfd, bufs[b].size,
               bufs[b].ino, snaps, dt);
        // Regions: runs of pages that changed at least once (gaps of up to 2 pages merged).
        size_t i = 0;
        int regions = 0;
        while (i < pages) {
            if (!changes[i]) {
                i++;
                continue;
            }
            size_t start = i, end = i, gap = 0;
            int most = 0;
            long total = 0;
            for (; i < pages; i++) {
                if (changes[i]) {
                    end = i, gap = 0;
                    total += changes[i];
                    if (changes[i] > most) most = changes[i];
                } else if (++gap > 2) {
                    break;
                }
            }
            size_t off = start * PAGE, len = (end - start + 1) * PAGE;
            double mean, nz;
            int lo, hi;
            stats(bufs[b].p + off, len, &mean, &lo, &hi, &nz);
            printf("  region 0x%08zx +0x%zx (%zu KiB): changed in up to %d of %d intervals (avg %.1f); "
                   "bytes mean %.1f min %d max %d nonzero %.0f%%\n",
                   off, len, len / 1024, most, snaps - 1, (double)total / (end - start + 1), mean, lo, hi, nz * 100);
            regions++;
        }
        if (!regions) {
            double mean, nz;
            int lo, hi;
            stats(bufs[b].p, bufs[b].size, &mean, &lo, &hi, &nz);
            printf("  no change; bytes mean %.1f min %d max %d nonzero %.1f%%\n", mean, lo, hi, nz * 100);
        }
        free(prev), free(cur), free(changes);
    }
}

#define EYE_W 512
#define EYE_H 400
#define EYE_SLOTS 8

static size_t slot_start(int k) {
    return 0x230000 + (size_t)k * 0x40000 + 0x40c0 + (size_t)k * 0x40 + (k >= 4 ? 0x40 : 0);
}

// A cheap fingerprint of a frame: 256 words spread over it (a new frame changes nearly all).
static uint64_t frame_sig(const uint8_t *p) {
    uint64_t h = 1469598103934665603ull, w;
    for (int i = 0; i < 256; i++) {
        memcpy(&w, p + (size_t)i * (EYE_W * EYE_H / 256), 8);
        h = (h ^ w) * 1099511628211ull;
    }
    return h;
}

static volatile sig_atomic_t stop_rec;
static void on_stop(int sig) { (void)sig; stop_rec = 1; }

// The poller copies finished frames into a ring; a writer thread saves them, so a slow disk
// write never delays the polling.
#define RING 128
static struct {
    uint8_t *frames;
    int slot[RING];
    double time[RING];
    size_t head, tail, dropped;  // head: next to fill (poller); tail: next to save (writer)
    int done;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    FILE *f, *ix;
} ring = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void *ring_writer(void *arg) {
    (void)arg;
    size_t fsize = EYE_W * EYE_H, n = 0;
    pthread_mutex_lock(&ring.mu);
    for (;;) {
        while (ring.tail == ring.head && !ring.done) pthread_cond_wait(&ring.cv, &ring.mu);
        if (ring.tail == ring.head) break;
        size_t i = ring.tail % RING;
        pthread_mutex_unlock(&ring.mu);
        fwrite(ring.frames + i * fsize, 1, fsize, ring.f);
        fprintf(ring.ix, "%zu %d %d %.6f\n", n++, ring.slot[i], ring.slot[i] >= 4, ring.time[i]);
        pthread_mutex_lock(&ring.mu);
        ring.tail++;
    }
    pthread_mutex_unlock(&ring.mu);
    return NULL;
}

static void ring_put(const uint8_t *frame, int slot, double t) {
    size_t fsize = EYE_W * EYE_H;
    pthread_mutex_lock(&ring.mu);
    int full = ring.head - ring.tail >= RING;
    pthread_mutex_unlock(&ring.mu);
    if (full) {
        ring.dropped++;
        return;
    }
    size_t i = ring.head % RING;
    memcpy(ring.frames + i * fsize, frame, fsize);
    ring.slot[i] = slot, ring.time[i] = t;
    pthread_mutex_lock(&ring.mu);
    ring.head++;
    pthread_cond_signal(&ring.cv);
    pthread_mutex_unlock(&ring.mu);
}

static int eye_buffer(void) {
    for (int i = 0; i < nbufs; i++)
        if (bufs[i].size == 16777216) return i;
    return -1;
}

// Calls done(frame, slot, time) for every complete eye-camera frame until `seconds` pass
// (forever if negative), a stop signal comes, the tracker process goes away, or keep()
// (checked about every 0.25 s, when given) says to stop. Looks every `poll_us`.
//
// A frame lands over several milliseconds, in bursts, and its last bursts can come after
// the camera has started its next frame. A slot isn't rewritten until at least three frames
// later (camera 0 cycles 3,0,1,2; camera 1 7,5,4,6,5,7,6,4), so a frame is passed on when
// its camera starts the frame after next. Changes to the slot just finished are late bursts,
// not a new frame. A frame's time is when its slot first changed.
//
// Each look checks only the slot each camera writes next: the one that followed the last two
// before (after[][], seeded with the orders above and learned as frames come). Every check
// reads 256 words spread over a frame in DMA-BUF memory, so all eight slots each time was
// most of the cost. A camera with nothing in its expected slot for 1.5 frames, or with no
// order known yet, has all its slots checked until a frame comes. When that finds a frame
// somewhere else (the order changed, or a frame was missed), all its slots are checked for
// SCAN_AFTER_MISS, as before, while after[][] learns the new order. A slot's fingerprint is
// taken again when it stops being one of the two in use, so a check later sees only a new frame.
// Between frames it sleeps until FRAME_EARLY before the next one is due (the cameras run at
// 90 fps, within a ms of each other), then looks every `poll_us`.
#define FRAME_DUE (1.5 / 90)  // s: a camera that hasn't started a frame by then gets all its slots checked
#define SCAN_AFTER_MISS 2.0    // s of checking all slots after a frame came in an unexpected one
#define CAMS_IDLE 0.5          // s without a frame from either camera: look 4 times less often
#define FRAME_EARLY 0.0025     // s before a frame is due to start looking for it

// When to start looking for the next frame: FRAME_EARLY before the first camera's is due. A
// camera already late (it lost its order, or stopped) means now.
static double next_due(const double last[2], double t) {
    double due = 1e300;
    for (int cam = 0; cam < 2; cam++) {
        double d = last[cam] + 1.0 / 90 - FRAME_EARLY;
        if (t - last[cam] > CAMS_IDLE) continue;  // stopped: the other one sets the pace
        if (d < due) due = d;
    }
    return due < 1e300 ? due : t;
}

static void poll_frames(int b, double seconds, int pid, void (*done)(const uint8_t *, int, double),
                        int (*keep)(void), unsigned poll_us) {
    static const int order[2][8] = {{3, 0, 1, 2, 3, 0, 1, 2}, {7, 5, 4, 6, 5, 7, 6, 4}};
    int after[EYE_SLOTS][EYE_SLOTS];
    memset(after, -1, sizeof after);
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < 8; i++) after[order[c][i]][order[c][(i + 1) % 8]] = order[c][(i + 2) % 8];
    uint64_t sig[EYE_SLOTS];
    double first[EYE_SLOTS];
    int cur[2] = {-1, -1}, prev[2] = {-1, -1};
    for (int k = 0; k < EYE_SLOTS; k++) sig[k] = frame_sig(bufs[b].p + slot_start(k)), first[k] = 0;
    double start = now(), checked = start, kept = start, last[2] = {start, start}, scan_until[2] = {0, 0};
    char proc[64];
    snprintf(proc, sizeof proc, "/proc/%d", pid);
    while ((seconds < 0 || now() - start < seconds) && !stop_rec) {
        double t = now();
        if (t - checked > 1.0) {  // the tracker restarted: its buffers are stale
            struct stat st;
            if (stat(proc, &st) != 0) return;
            checked = t;
        }
        if (keep && t - kept > 0.25) {
            if (!keep()) return;
            kept = t;
        }
        for (int cam = 0; cam < 2; cam++) {
            int next = prev[cam] >= 0 ? after[prev[cam]][cur[cam]] : -1;
            int all = next < 0 || t - last[cam] > FRAME_DUE || t < scan_until[cam];
            int from = all ? cam * 4 : next, to = all ? cam * 4 + 4 : next + 1;
            for (int k = from; k < to; k++) {
                uint64_t s = frame_sig(bufs[b].p + slot_start(k));
                if (s == sig[k]) continue;
                sig[k] = s;
                if (k == cur[cam] || k == prev[cam]) continue;  // landing, or a late burst
                if (next >= 0 && k != next) scan_until[cam] = t + SCAN_AFTER_MISS;
                if (prev[cam] >= 0) {
                    done(bufs[b].p + slot_start(prev[cam]), prev[cam], first[prev[cam]]);
                    after[prev[cam]][cur[cam]] = k;
                    // Out of use from now on: later changes are a new frame.
                    sig[prev[cam]] = frame_sig(bufs[b].p + slot_start(prev[cam]));
                }
                prev[cam] = cur[cam];
                cur[cam] = k;
                first[k] = t;
                last[cam] = t;
                next = prev[cam] >= 0 ? after[prev[cam]][cur[cam]] : -1;
            }
        }
        double wait = poll_us * 1e-6;
        if (t - last[0] > CAMS_IDLE && t - last[1] > CAMS_IDLE) {
            wait *= 4;
        } else if (next_due(last, t) - t > wait) {
            wait = next_due(last, t) - t;
        }
        usleep((useconds_t)(wait * 1e6));
    }
}

static void rec_frame(const uint8_t *frame, int slot, double t) { ring_put(frame, slot, t); }

static int rec(double seconds, const char *dir, int pid) {
    int b = eye_buffer();
    if (b < 0) {
        fprintf(stderr, "no 16 MiB buffer\n");
        return 1;
    }
    // Frames stream to disk (about 37 MB/s), so a long recording doesn't fill memory.
    // Ctrl-C or SIGTERM ends it early and keeps what was recorded.
    char path[512];
    snprintf(path, sizeof path, "%s/frames.raw", dir);
    ring.f = create_private(path, "wb");
    snprintf(path, sizeof path, "%s/index.txt", dir);
    ring.ix = create_private(path, "w");
    ring.frames = malloc((size_t)RING * EYE_W * EYE_H);
    if (!ring.f || !ring.ix || !ring.frames) {
        perror(dir);
        return 1;
    }
    setvbuf(ring.f, NULL, _IOFBF, 4 << 20);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    pthread_t writer;
    pthread_create(&writer, NULL, ring_writer, NULL);
    double start = now();
    poll_frames(b, seconds, pid, rec_frame, NULL, 300);  // 0.3 ms: recordings' times
    pthread_mutex_lock(&ring.mu);
    ring.done = 1;
    pthread_cond_signal(&ring.cv);
    pthread_mutex_unlock(&ring.mu);
    pthread_join(writer, NULL);
    fclose(ring.f), fclose(ring.ix);
    printf("%zu frames in %.1f s to %s", ring.head, now() - start, dir);
    if (ring.dropped) printf(" (%zu dropped: disk too slow)", ring.dropped);
    printf("\n");
    free(ring.frames);
    return 0;
}

// --- --share: the latest frames in shared memory for the live tracker ---
//
// The file (SHARE_PATH, mode 0600, owned by the --owner user) is a header, then SHARE_SLOTS
// entries per camera. Each entry is a 64-byte head and one 512x400 frame. Frame n of camera
// c goes in entry c * SHARE_SLOTS + n % SHARE_SLOTS. The head's `seq` is odd while it's
// written (read it before and after copying, and retry if it changed or was odd), and
// count[c] is how many frames camera c has published. tracker_pid is 0 while no frames
// come (nobody wants them, or SteamVR's tracker isn't running).
//
// This keeps the tracker's own buffers behind root: the user side only ever sees copies.
#define SHARE_SLOTS 8
#define SHARE_MAGIC 0x31434546u  // "FEC1"
#define WANT_FRESH 3.0           // seconds a touch of the --want file lasts
// How often --share looks for frames, with a timer slack that lets the kernel group the
// wakeups: a frame reaches ft-eyes 1 to 1.5 ms after its camera starts the next, not 0.3.
#define SHARE_POLL_US 1000
#define SHARE_SLACK_NS 500000

typedef struct {
    uint32_t magic, version, width, height, slots, entry_size;
    volatile uint64_t count[2];
    uint32_t tracker_pid, pad0;
    uint8_t pad[16];
} share_head_t;

typedef struct {
    volatile uint64_t seq;
    double t;
    uint64_t n;
    uint32_t cam, slot;
    uint8_t pad[32];
} share_entry_t;

_Static_assert(sizeof(share_head_t) == 64, "share header");
_Static_assert(sizeof(share_entry_t) == 64, "share entry");

static uint8_t *share;
static const char *want_path;
static uid_t owner_uid = (uid_t)-1;
static gid_t owner_gid = (gid_t)-1;

static void share_frame(const uint8_t *frame, int slot, double t) {
    share_head_t *h = (share_head_t *)share;
    int cam = slot >= 4;
    uint64_t n = h->count[cam];
    size_t esize = sizeof(share_entry_t) + EYE_W * EYE_H;
    share_entry_t *e = (share_entry_t *)(share + sizeof *h + (cam * SHARE_SLOTS + n % SHARE_SLOTS) * esize);
    e->seq++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy((uint8_t *)(e + 1), frame, EYE_W * EYE_H);
    e->t = t, e->n = n, e->cam = cam, e->slot = slot;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e->seq++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    h->count[cam] = n + 1;
}

// Someone reads the frames: the want file was touched lately. It must be a regular file
// (lstat: a link isn't followed) owned by the frames' owner, so no one else can turn this on.
static int wanted(void) {
    if (!want_path) return 1;
    struct stat st;
    if (lstat(want_path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (owner_uid != (uid_t)-1 && st.st_uid != owner_uid) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double age = (ts.tv_sec - st.st_mtim.tv_sec) + (ts.tv_nsec - st.st_mtim.tv_nsec) * 1e-9;
    return age < WANT_FRESH;
}

static void close_bufs(void) {
    for (int i = 0; i < nbufs; i++) munmap((void *)bufs[i].p, bufs[i].size), close(bufs[i].fd);
    nbufs = 0;
}

static int share_loop(const char *path) {
    size_t esize = sizeof(share_entry_t) + EYE_W * EYE_H;
    size_t size = sizeof(share_head_t) + 2 * SHARE_SLOTS * esize;
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, size) != 0) {
        perror(path);
        return 1;
    }
    if (owner_uid != (uid_t)-1 && fchown(fd, owner_uid, owner_gid) != 0) perror("fchown");
    share = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (share == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    share_head_t *h = (share_head_t *)share;
    *h = (share_head_t){.magic = SHARE_MAGIC, .version = 1, .width = EYE_W, .height = EYE_H,
                        .slots = SHARE_SLOTS, .entry_size = (uint32_t)esize};
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    if (prctl(PR_SET_TIMERSLACK, SHARE_SLACK_NS, 0, 0, 0) != 0) perror("PR_SET_TIMERSLACK");
    fprintf(stderr, "ft-eyegrab: sharing frames in %s%s%s\n", path, want_path ? " while wanted by " : "",
            want_path ? want_path : "");
    int pid = -1, idle = -1, missing = 0;
    while (!stop_rec) {
        if (!wanted()) {
            // Nobody reads the frames: copy nothing, and let go of the tracker's buffers.
            if (idle != 1) fprintf(stderr, "ft-eyegrab: idle (nobody wants frames)\n"), idle = 1;
            close_bufs();
            pid = -1;
            h->tracker_pid = 0;
            usleep(250000);
            continue;
        }
        if (nbufs == 0 && ((pid = find_tracker()) < 0 || open_bufs(pid) <= 0 || eye_buffer() < 0)) {
            // SteamVR's tracker isn't running (yet, or again).
            if (!missing) fprintf(stderr, "ft-eyegrab: waiting for SteamVR's eyetracking\n"), missing = 1;
            close_bufs();
            h->tracker_pid = 0;
            sleep(2);
            continue;
        }
        if (idle != 0 || missing) fprintf(stderr, "ft-eyegrab: copying frames from eyetracking %d\n", pid);
        idle = 0, missing = 0;
        h->tracker_pid = pid;
        poll_frames(eye_buffer(), -1, pid, share_frame, wanted, SHARE_POLL_US);
        struct stat st;
        char proc[64];
        snprintf(proc, sizeof proc, "/proc/%d", pid);
        if (!stop_rec && stat(proc, &st) != 0) {
            fprintf(stderr, "ft-eyegrab: eyetracking %d went away; waiting for it\n", pid);
            close_bufs();
            h->tracker_pid = 0;
        }
    }
    close_bufs();
    unlink(path);
    return 0;
}

static int dump(int b, size_t off, size_t len, const char *file) {
    if (b < 0 || b >= nbufs || off + len > bufs[b].size) {
        fprintf(stderr, "out of range\n");
        return 1;
    }
    FILE *f = create_private(file, "wb");
    if (!f) {
        perror(file);
        return 1;
    }
    fwrite(bufs[b].p + off, 1, len, f);
    fclose(f);
    printf("wrote %zu bytes to %s\n", len, file);
    return 0;
}

static int seq(int b, size_t off, size_t len, int frames, const char *dir) {
    if (b < 0 || b >= nbufs || off + len > bufs[b].size) {
        fprintf(stderr, "out of range\n");
        return 1;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/times.txt", dir);
    FILE *times = create_private(path, "w");
    if (!times) {
        perror(path);
        return 1;
    }
    uint8_t *copy = malloc(len);
    uint64_t last = 0;
    int got = 0;
    double start = now();
    while (got < frames && now() - start < 30) {
        uint64_t h = 0;
        for (size_t i = 0; i + PAGE <= len; i += PAGE * 8) h ^= page_hash(bufs[b].p + off + i) + i;
        if (h != last) {
            last = h;
            double t = now();
            memcpy(copy, bufs[b].p + off, len);
            snprintf(path, sizeof path, "%s/%04d.raw", dir, got);
            FILE *f = create_private(path, "wb");
            if (f) fwrite(copy, 1, len, f), fclose(f);
            fprintf(times, "%d %.6f\n", got, t);
            got++;
        }
        usleep(1000);
    }
    fclose(times);
    free(copy);
    printf("%d frames in %s\n", got, dir);
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: ft-eyegrab [--share PATH [--owner UID:GID] [--want FILE] | --scan [N] | --dump I OFF LEN FILE |\n"
                    "                   --seq I OFF LEN FRAMES DIR | --rec SECONDS DIR]\n");
}

#ifndef FT_EYEGRAB_NO_MAIN
int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--share") == 0) {
        const char *uid = getenv("SUDO_UID"), *gid = getenv("SUDO_GID");
        if (uid && gid) owner_uid = (uid_t)atoi(uid), owner_gid = (gid_t)atoi(gid);
        for (int i = 3; i < argc; i++) {
            unsigned u, g;
            if (strcmp(argv[i], "--owner") == 0 && i + 1 < argc && sscanf(argv[i + 1], "%u:%u", &u, &g) == 2) {
                owner_uid = u, owner_gid = g, i++;
            } else if (strcmp(argv[i], "--want") == 0 && i + 1 < argc) {
                want_path = argv[++i];
            } else {
                usage();
                return 2;
            }
        }
        return share_loop(argv[2]);
    }
    int pid = find_tracker();
    if (pid < 0) {
        fprintf(stderr, "SteamVR's eyetracking process isn't running\n");
        return 1;
    }
    if (open_bufs(pid) <= 0) {
        fprintf(stderr, "no buffers (run as root)\n");
        return 1;
    }
    if (argc >= 2 && strcmp(argv[1], "--scan") == 0) {
        scan(argc >= 3 ? atoi(argv[2]) : 40);
    } else if (argc == 6 && strcmp(argv[1], "--dump") == 0) {
        return dump(atoi(argv[2]), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0), argv[5]);
    } else if (argc == 4 && strcmp(argv[1], "--rec") == 0) {
        return rec(atof(argv[2]), argv[3], pid);
    } else if (argc == 7 && strcmp(argv[1], "--seq") == 0) {
        return seq(atoi(argv[2]), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0), atoi(argv[5]), argv[6]);
    } else if (argc == 1) {
        printf("eyetracking pid %d\n", pid);
        for (int b = 0; b < nbufs; b++)
            printf("buffer %d: fd %d, %zu bytes, ino %lu\n", b, bufs[b].xfd, bufs[b].size, bufs[b].ino);
    } else {
        usage();
        return 2;
    }
    return 0;
}
#endif
