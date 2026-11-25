// Unit tests for the mouse having the pointer (pointer/helper/mousehas.h): what the helper tells the
// relay, which then skips its wake guard.
// Build and run: just test-c (or g++ -std=c++17 -I. pointer/tests/mousehas-test.cpp).
#include "pointer/helper/mousehas.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what);     \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

int main() {
    using mousehas::Has;
    CHECK(Has(true, false, false, false), "the pointer on, nothing else driving it: the mouse's");
    CHECK(!Has(false, false, false, false), "the pointer off (idle, a controller moved, the headset off): to be woken");
    CHECK(!Has(true, true, false, false), "gaze mode: the eyes aim, a move takes the pointer from them");
    CHECK(!Has(true, false, true, false), "a hand's ray drives it: a move drops the ray");
    CHECK(!Has(true, false, false, true), "a hand's or the head's hold steers it");
    if (failures) return std::printf("%d failure(s)\n", failures), 1;
    std::printf("mousehas-test: ok\n");
    return 0;
}
