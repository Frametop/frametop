// ft-eyegrab's lab files (--rec, --seq, --dump), offline: create_private makes them 0600 whatever
// the umask, rewrites an old file as 0600, and refuses a link or a file with other names, leaving
// the target as it was. Builds ft-eyegrab.c without its main; no camera, SteamVR or root needed.
#define FT_EYEGRAB_NO_MAIN
#include "../ft-eyegrab.c"

#include <assert.h>

static char dir[] = "/tmp/eyegrab-files-test-XXXXXX";

static char *at(const char *name) {
    static char path[2][256];
    static int i;
    i ^= 1;
    snprintf(path[i], sizeof path[i], "%s/%s", dir, name);
    return path[i];
}

static mode_t mode_of(const char *path) {
    struct stat st;
    assert(lstat(path, &st) == 0);
    return st.st_mode & 07777;
}

static void write_file(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(fchmod(fd, mode) == 0);
    close(fd);
}

static void expect_text(const char *path, const char *text) {
    char got[64] = {0};
    FILE *f = fopen(path, "r");
    assert(f);
    size_t n = fread(got, 1, sizeof got - 1, f);
    fclose(f);
    assert(n == strlen(text) && memcmp(got, text, n) == 0);
}

int main(void) {
    assert(mkdtemp(dir));
    umask(0);  // the mode must not come from the umask

    FILE *f = create_private(at("frames.raw"), "wb");
    assert(f);
    fputs("new", f);
    fclose(f);
    assert(mode_of(at("frames.raw")) == 0600);
    expect_text(at("frames.raw"), "new");

    // An old file from an earlier run is emptied and made private.
    write_file(at("index.txt"), "old contents", 0644);
    f = create_private(at("index.txt"), "w");
    assert(f);
    fputs("x", f);
    fclose(f);
    assert(mode_of(at("index.txt")) == 0600);
    expect_text(at("index.txt"), "x");

    // A link to another file is refused, and the target is left alone.
    write_file(at("target"), "keep", 0644);
    assert(symlink(at("target"), at("link")) == 0);
    errno = 0;
    assert(create_private(at("link"), "wb") == NULL && errno == ELOOP);
    expect_text(at("target"), "keep");
    assert(mode_of(at("target")) == 0644);

    // So is a file with a second name.
    assert(link(at("target"), at("hard")) == 0);
    errno = 0;
    assert(create_private(at("hard"), "wb") == NULL && errno == EPERM);
    expect_text(at("target"), "keep");
    assert(mode_of(at("target")) == 0644);

    printf("ok\n");
    return 0;
}
