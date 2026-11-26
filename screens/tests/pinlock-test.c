#define _GNU_SOURCE
#include "../../conf/ft_pinlock.h"
#include <assert.h>
#include <limits.h>

int main(void) {
    assert(ft_pin_fresh(900, 900, 1000, 1000));
    assert(!ft_pin_fresh(250, 250, 1000, 1000));
    assert(!ft_pin_fresh(900, 900, 1300, 1000));
    assert(!ft_pin_fresh(1100, 900, 1000, 1000));
    assert(!ft_pin_fresh(900, 1100, 1000, 1000));
    assert(!ft_pin_fresh(-1, 900, 1000, 1000));
    assert(!ft_pin_fresh(LLONG_MAX, 900, 1000, 1000));
    assert(!ft_pin_fresh(900, 900, -1, 1000));
    char path[] = "/tmp/ft-pin-lease-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    char wire[160];
    const int n = snprintf(wire, sizeof wire, "FTPIN1 1 %lld %lld %d\n",
                           (long long)ft_pin_ms(CLOCK_BOOTTIME),
                           (long long)ft_pin_ms(CLOCK_MONOTONIC), (int)getpid());
    assert(write(fd, wire, n) == n);
    assert(ft_pin_read(path));
    assert(!fchmod(fd, 0644));
    assert(!ft_pin_read(path));
    assert(!fchmod(fd, 0600));
    assert(!ftruncate(fd, 0));
    assert(!ft_pin_read(path));
    close(fd);
    unlink(path);
    assert(!ft_pin_read(path));
    puts("ok PIN lease: freshness, expiry, suspend and invalid clocks");
    return 0;
}
