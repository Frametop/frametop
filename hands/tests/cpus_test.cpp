// parse_cpus (track/cpus.h): lists, ranges, and every malformed or out-of-range list refused as a whole.
#include "../track/cpus.h"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void Check(const char *in, std::vector<int> want) {
    if (parse_cpus(in) != want) std::printf("FAILED: \"%s\"\n", in), ++failures;
}
}  // namespace

int main() {
    Check("5,6,7", {5, 6, 7});
    Check("5-7", {5, 6, 7});
    Check("0,5-7", {0, 5, 6, 7});
    Check("3", {3});
    Check("", {});
    Check("5,", {});
    Check(",5", {});
    Check("5,,6", {});
    Check("7-5", {});
    Check("5-", {});
    Check("-1", {});
    Check("5,-7", {});
    Check("5x", {});
    Check(" 5", {});
    Check("+5", {});
    Check("1024", {});
    Check("1023", {1023});
    Check("99999999999999999999", {});
    std::printf(failures ? "cpus: %d failed\n" : "cpus: ok\n", failures);
    return failures != 0;
}
