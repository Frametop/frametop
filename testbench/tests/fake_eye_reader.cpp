// Reads the testbench's fake eye files with ft-gaze's own code (EyeFile, Detect, ReadSample,
// OwnFile in gaze/ft-gaze.cpp), from $XDG_RUNTIME_DIR/frametop-testbench/ like ft-gaze does,
// and prints what it got for test_fakes.py, which keeps writing them meanwhile.
// With "stale": the files as a crashed testbench leaves them (written 5 s ago, by this), and
// whether ft-gaze then goes back to the real trackers' files.
#define main ft_gaze_main
#include "../../gaze/ft-gaze.cpp"
#undef main

namespace {
void Vec(const char *name, Vec3 v) { std::printf("\"%s\":[%.9g,%.9g,%.9g],", name, v.x, v.y, v.z); }
void Floats(const char *name, const float *v, int n) {
    std::printf("\"%s\":[", name);
    for (int i = 0; i < n; ++i) std::printf("%s%.9g", i ? "," : "", v[i]);
    std::printf("],");
}
}  // namespace

int Stale() {
    const std::string eyes = TestbenchPath("eye-server.mmap"), own = TestbenchPath("frametop-eyes-gaze");
    OwnFile file;
    OwnSample o;
    file.Read(o);
    std::printf("{\"before\":{\"eyes\":%d,\"own\":%d,\"path\":\"%s\"},", Fresh(eyes), Fresh(own),
                file.Path().c_str());
    timespec old[2];
    clock_gettime(CLOCK_REALTIME, &old[0]);
    old[0].tv_sec -= 5;
    old[1] = old[0];
    utimensat(AT_FDCWD, eyes.c_str(), old, 0);
    utimensat(AT_FDCWD, own.c_str(), old, 0);
    usleep(2'100'000);  // OwnFile looks again every 2 s
    file.Read(o);
    std::printf("\"after\":{\"eyes\":%d,\"own\":%d,\"path\":\"%s\"}}\n", Fresh(eyes), Fresh(own),
                file.Path().c_str());
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "stale") return Stale();
    const std::string path = TestbenchPath("eye-server.mmap");
    EyeFile eyes;
    const bool fresh = Fresh(path), opened = eyes.Open(path.c_str());
    EyeFile::Detection d = EyeFile::kNone;
    for (const double end = NowRaw() + 2.0; opened && NowRaw() < end;) {
        if ((d = eyes.Detect(NowRaw())) == EyeFile::kFound) break;
        usleep(4000);
    }
    EyeSample s;
    bool read = false;
    for (int i = 0; i < 50 && d == EyeFile::kFound && !(read = ReadSample(eyes, s)); ++i) usleep(1000);
    std::printf("{\"fresh\":%d,\"opened\":%d,\"found\":%d,\"shift\":%zu,\"read\":%d,\"age\":%.6f,", fresh, opened,
                d == EyeFile::kFound, eyes.shift, read, NowRaw() - s.t);
    Vec("left1", s.left1), Vec("right1", s.right1), Vec("left2", s.left2), Vec("right2", s.right2);
    Vec("fix1", s.fix1);
    Floats("open", s.open, 2), Floats("var1", s.var1, 6), Floats("var2", s.var2, 6), Floats("meas", s.meas, 8);
    OwnFile own;
    OwnSample o;
    const bool ownRead = own.Read(o);
    std::printf("\"own\":{\"read\":%d,\"age\":%.6f,\"yaw\":%.9g,\"pitch\":%.9g,\"flags\":%u,", ownRead, NowRaw() - o.t,
                o.yaw, o.pitch, o.flags);
    Floats("eyes", o.eyes, 4);
    std::printf("\"n\":%u}}\n", o.n);
    return 0;
}
