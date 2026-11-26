// The ARM generic timer's count (CNTVCT_EL0, 19.2 MHz on the Frame): the same reference clock the DSP wrapper
// stamps worker 0's start and end with (status words 10..13), so NPUD_PHASES can split a request's wait into the
// DSP's wake, its kernel and the host's notice of completion. Measurement only.
#include <stdint.h>

uint64_t ticks(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
