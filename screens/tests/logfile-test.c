// ft-screens' log file (screens/logfile.h): appended to, rotated by size, at most KEEP old ones.
#define _GNU_SOURCE
#include "../logfile.h"

#include <stdlib.h>
#include <string.h>

static int failed = 0;
static FILE *out;  // the real stdout: the test moves stdout to the log

static void check(const char *what, int ok) {
    failed += !ok;
    fprintf(out, "%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static long size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int holds(const char *path, const char *text) {
    char b[4096] = {0};
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    const size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return strstr(b, text) != NULL;
}

static void fill(const char *path, const char *text, int bytes) {
    FILE *f = fopen(path, "a");
    fputs(text, f);
    for (int i = (int)strlen(text); i < bytes; ++i) fputc('.', f);
    fputc('\n', f);
    fclose(f);
}

int main(void) {
    out = fdopen(dup(STDOUT_FILENO), "w");
    setvbuf(out, NULL, _IONBF, 0);
    char dir[] = "/tmp/logfile-test-XXXXXX", log[200], one[256], two[256], three[256];
    if (!mkdtemp(dir)) return 2;
    snprintf(log, sizeof log, "%s/screens.log", dir);
    snprintf(one, sizeof one, "%s.1", log);
    snprintf(two, sizeof two, "%s.2", log);
    snprintf(three, sizeof three, "%s.3", log);
    const long cap = 100;

    check("no log yet: nothing to rotate", !ft_log_rotate(log, cap, 2));
    fill(log, "first run", 50);
    check("under the cap: kept as it is", !ft_log_rotate(log, cap, 2) && size_of(log) == 51);
    check("stdout goes to it, appended", ft_log_open(log) == 0);
    printf("second run\n");
    fflush(stdout);
    check("...after what was there", holds(log, "first run") && holds(log, "second run"));

    fill(log, "past the cap", 120);
    ft_log_check(log, cap, 2);
    printf("after the check\n");
    fflush(stdout);
    check("past the cap: the log becomes .1", holds(one, "first run") && holds(one, "past the cap"));
    check("...and stdout goes on in a new one", holds(log, "after the check") && !holds(log, "first run"));

    fill(log, "third", 120);
    ft_log_check(log, cap, 2);
    fill(log, "fourth", 120);
    ft_log_check(log, cap, 2);
    check("older ones move down", holds(one, "fourth") && holds(two, "third"));
    check("no more than KEEP old ones", size_of(three) < 0);
    check("ft_log_check without a path does nothing", (ft_log_check(NULL, cap, 2), 1));

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) return 2;
    fprintf(out, failed ? "%d failed\n" : "all passed\n", failed);
    return failed ? 1 : 0;
}
