#include "park.h"

#include <sys/stat.h>
#include <time.h>

ParkState park_decide(const ParkInputs &in) {
    if (in.recording) return {};
    if (in.testbench) return {true, false, ParkWhy::testbench};
    if (in.park && !in.needed) return {true, false, ParkWhy::park};
    if (in.held) return {true, true, ParkWhy::other};
    return {};
}

bool file_fresh(const std::string &path) {
    struct stat st;
    timespec now;
    if (stat(path.c_str(), &st) != 0 || clock_gettime(CLOCK_REALTIME, &now) != 0) return false;
    const double age = double(now.tv_sec - st.st_mtim.tv_sec) + (now.tv_nsec - st.st_mtim.tv_nsec) * 1e-9;
    return age > -3.0 && age < 3.0;
}
