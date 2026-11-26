// track/park.cpp: when ft-hands parks, from the files that ask for it (track/park.h), and what "fresh" means.
//   build/park-test
#include "../track/park.h"
#include "../track/pidfile.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures; \
    } while (0)

namespace {

ParkState decide(bool park, bool testbench, bool held, bool needed, bool recording) {
    ParkInputs in;
    in.park = park, in.testbench = testbench, in.held = held, in.needed = needed, in.recording = recording;
    return park_decide(in);
}

void decisions() {
    CHECK(!decide(false, false, false, false, false).parked);
    // the relay parks it for a VR game, and hands-want says so to ft-camd (not lent)
    const ParkState p = decide(true, false, false, false, false);
    CHECK(p.parked && !p.lent && p.why == ParkWhy::park);
    // the Hand Recorder needs the hands: the relay's park doesn't hold
    CHECK(!decide(true, false, false, true, false).parked);
    // the testbench and another producer still park it, needed or not
    CHECK(decide(true, true, false, true, false).why == ParkWhy::testbench);
    const ParkState o = decide(true, false, true, true, false);
    CHECK(o.parked && o.lent && o.why == ParkWhy::other);
    CHECK(decide(false, false, true, false, false).lent);
    // the testbench wins over another producer, and leaves hands-want to the parked loop
    const ParkState t = decide(false, true, true, false, false);
    CHECK(t.parked && !t.lent && t.why == ParkWhy::testbench);
    // never while recording
    CHECK(!decide(true, true, true, false, true).parked);
}

void freshness() {
    char tmpl[] = "/tmp/park-test-XXXXXX";
    const std::string dir = mkdtemp(tmpl), f = dir + "/needed";
    CHECK(!file_fresh(f));   // missing
    std::ofstream(f) << "";
    CHECK(file_fresh(f));
    timeval old[2] = {};
    gettimeofday(&old[0], nullptr);
    old[0].tv_sec -= 5, old[1] = old[0];
    utimes(f.c_str(), old);
    CHECK(!file_fresh(f));   // older than 3 s: a reader that died lets go
    unlink(f.c_str());
    rmdir(dir.c_str());
}

void pidfiles() {
    char tmpl[] = "/tmp/park-test-XXXXXX";
    const std::string dir = mkdtemp(tmpl), f = dir + "/hands.pid";
    CHECK(pidfile_write(f));
    long pid = 0;
    std::ifstream(f) >> pid;
    CHECK(pid == long(getpid()));
    std::ofstream(f) << "1\n";   // a newer ft-hands wrote it: not ours to remove
    pidfile_remove(f);
    CHECK(access(f.c_str(), F_OK) == 0);
    CHECK(pidfile_write(f));
    pidfile_remove(f);
    CHECK(access(f.c_str(), F_OK) != 0);
    const std::string link = dir + "/link.pid";
    CHECK(symlink("/tmp/park-test-elsewhere", link.c_str()) == 0);
    CHECK(!pidfile_write(link));   // a link at the path is refused
    CHECK(access("/tmp/park-test-elsewhere", F_OK) != 0);
    unlink(link.c_str());
    const std::string sub = dir + "/frametop-hands", early = sub + "/hands.pid";
    CHECK(pidfile_write(early));   // before the run dir is made: it makes it
    struct stat st;
    CHECK(stat(sub.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    pidfile_remove(early);
    rmdir(sub.c_str());
    rmdir(dir.c_str());
}

}  // namespace

int main() {
    decisions();
    freshness();
    pidfiles();
    std::printf(failures ? "park: %d failures\n" : "park: ok\n", failures);
    return failures != 0;
}
