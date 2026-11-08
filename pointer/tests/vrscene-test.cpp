// conf/vrscene.h without SteamVR: which scene apps are Steam Link's client, and the relay's
// PC game flag (fresh, stale, "0", missing). pointer/tests/vrscene-test.cpp
#include "conf/vrscene.h"

#include <cstdio>
#include <string>

static int failures = 0;
static void Check(const char *label, bool got, bool want) {
    std::printf("%s%s\n", got == want ? "ok    " : "FAIL  ", label);
    if (got != want) ++failures;
}

int main() {
    using ftscene::Kind;
    Check("app key vrlink.client: Steam Link", ftscene::IsLink("vrlink.client", "anything"), true);
    Check("another app key: a game", ftscene::IsLink("steam.app.620980", "vrlink"), false);
    Check("no key, process vrlink: Steam Link", ftscene::IsLink("", "vrlink"), true);
    Check("no key, another process: a game", ftscene::IsLink("", "BeatSaber"), false);
    Check("no key, no process: a game", ftscene::IsLink(nullptr, nullptr), false);

    Check("a game gives way", ftscene::GivesWay(Kind::Game, false), true);
    Check("Steam Link alone: Frametop stays", ftscene::GivesWay(Kind::Link, false), false);
    Check("Steam Link with a PC game: gives way", ftscene::GivesWay(Kind::Link, true), true);
    Check("no scene app: stays, whatever the flag", ftscene::GivesWay(Kind::None, true), false);

    char dir[] = "/tmp/vrscene-test-XXXXXX";
    if (!mkdtemp(dir)) return std::perror("mkdtemp"), 1;
    const std::string path = std::string(dir) + "/frametop-pcgame";
    Check("no file: no PC game", ftscene::PcGameAt(path, 1e9), false);
    auto write = [&](const char *text) {
        FILE *f = std::fopen(path.c_str(), "w");
        std::fputs(text, f);
        std::fclose(f);
    };
    struct stat st{};
    write("1\n");
    stat(path.c_str(), &st);
    const double t = st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
    Check("fresh \"1\": a PC game", ftscene::PcGameAt(path, t + 1), true);
    Check("\"1\" 16 s old: the relay stopped, ignored", ftscene::PcGameAt(path, t + 16), false);
    write("0\n");
    Check("fresh \"0\": none", ftscene::PcGameAt(path, t + 1), false);
    unlink(path.c_str());
    rmdir(dir);
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    Check("path in XDG_RUNTIME_DIR", ftscene::PcGamePath() == "/run/user/1000/frametop-pcgame", true);
    unsetenv("XDG_RUNTIME_DIR");
    Check("path without it: /tmp", ftscene::PcGamePath() == "/tmp/frametop-pcgame", true);
    Check("names", std::string(ftscene::Name(Kind::Link)) == "link" && std::string(ftscene::Name(Kind::None)) == "none", true);
    return failures ? 1 : 0;
}
