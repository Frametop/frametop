// The avatars' files on a Frame (together/local/wardrobe.h): which of the wearer's models the
// room may hear of, and the files that came from the others, kept in a folder that is this
// user's alone and gone with the session. Real files, in folders made here.
#include "../../avatar/modelfile.h"
#include "../local/wardrobe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static std::string g_avatars, g_run;

static std::string path(const std::string &name) { return g_avatars + "/" + name + ".ftmodel"; }

// A model's file in the avatars folder: `filler` more bytes in a chunk no reader knows.
static std::vector<uint8_t> model(const std::string &name, bool mayShare, size_t filler = 100, uint8_t seed = 1) {
    modelfile::Model m;
    m.info.name = name, m.info.licence = "CC0", m.info.mayShare = mayShare;
    modelfile::Extra more = {{'J', 'U', 'N', 'K'}, std::vector<uint8_t>(filler)};
    for (size_t i = 0; i < filler; ++i) more.bytes[i] = uint8_t(seed + i * 7);
    const std::vector<uint8_t> bytes = modelfile::Write(m, {more});
    FILE *f = std::fopen(path(name).c_str(), "wb");
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return bytes;
}

static std::vector<uint8_t> slurp(const std::string &file) {
    std::vector<uint8_t> out;
    std::string err;
    modelfile::ReadBytes(file, out, err);
    return out;
}

static size_t files(const std::string &dir) {
    size_t n = 0;
    if (DIR *d = opendir(dir.c_str())) {
        while (const dirent *e = readdir(d)) n += e->d_name[0] != '.';
        closedir(d);
    }
    return n;
}

static proto::Look lookOf(const std::vector<uint8_t> &bytes) { return flow::LookOf(bytes.data(), bytes.size()); }

static void wearing() {
    local::Wardrobe w("test");
    proto::Look look;
    const std::vector<uint8_t> fox = model("fox", true), secret = model("secret", false);
    bool ok = w.Wear("fox", look).empty() && proto::SameLook(look, lookOf(fox)) && w.Worn(look) && *w.Worn(look) == fox;
    check("a model marked as one that may be sent is worn for the room to hear of, and its file is the one to send", ok);
    proto::Room room;
    check("...and says so", w.State(room, -1) == "{\"wear\":\"fox\",\"sent\":true,\"why\":\"\",\"people\":[]}");
    const proto::Look foxLook = look;
    std::string why = w.Wear("secret", look);
    ok = why.find("isn't marked") != std::string::npos && look.none() && !w.Worn(foxLook) && !w.Worn(lookOf(secret));
    check("one that isn't marked is worn in private: the room hears of none, and no file is sent", ok && w.State(room, -1).find("\"wear\":\"secret\",\"sent\":false,\"why\":\"that avatar isn't marked") != std::string::npos);
    ok = !w.Wear("nobody", look).empty() && look.none() && !w.Wear("../fox", look).empty() && look.none() && !w.Wear("Fox", look).empty() && !w.Wear("fox.ftmodel", look).empty() && look.none();
    check("no such model, or a name that isn't a model's, is no avatar", ok);
    FILE *f = std::fopen(path("junk").c_str(), "wb");
    std::fputs("FTMD and then nothing a model has", f);
    std::fclose(f);
    mkdir(path("folder").c_str(), 0700);
    ok = w.Wear("junk", look).find("can't be read") != std::string::npos && look.none() && !w.Wear("folder", look).empty() && look.none();
    check("nor is a file that isn't a model, or a folder", ok);
    model("huge", true, proto::kMostLook);
    const std::vector<uint8_t> most = model("most", true, proto::kMostLook - 200);
    ok = w.Wear("huge", look).find("too large") != std::string::npos && look.none() && most.size() <= proto::kMostLook && w.Wear("most", look).empty() && look.size == most.size();
    check("a model larger than a look may be isn't sent; the largest that fits is", ok);
    check("no model at all is no avatar, and nothing is wrong with that", w.Wear("", look).empty() && look.none() && w.State(room, -1).find("\"wear\":\"\",\"sent\":false,\"why\":\"\"") != std::string::npos);

    // the file is read again only when it changed
    ok = w.Wear("fox", look).empty() && proto::SameLook(look, foxLook);
    struct stat was;
    stat(path("fox").c_str(), &was);
    std::vector<uint8_t> other = fox;
    other.back() ^= 0xff;
    int fd = open(path("fox").c_str(), O_WRONLY);   // in place, and dated back: nothing says it changed
    ok = ok && write(fd, other.data(), other.size()) == ssize_t(other.size());
    const timespec times[2] = {was.st_atim, was.st_mtim};
    futimens(fd, times);
    close(fd);
    ok = ok && w.Wear("fox", look).empty() && proto::SameLook(look, foxLook) && *w.Worn(look) == fox;
    check("asked again, a file that didn't change isn't read again", ok);
    const std::vector<uint8_t> fox2 = model("fox", true, 300, 9);
    ok = w.Wear("fox", look).empty() && proto::SameLook(look, lookOf(fox2)) && !w.Worn(foxLook) && *w.Worn(look) == fox2;
    check("...and one that did is, and is another look", ok);
    unlink(path("fox").c_str());
    check("a model that was taken away is off", !w.Wear("fox", look).empty() && look.none() && !w.Worn(lookOf(fox2)));
}

static void keeping() {
    const std::string dir = g_run + "/test-looks";
    std::vector<std::vector<uint8_t>> came;
    for (uint8_t i = 0; i < 6; ++i) came.push_back(std::vector<uint8_t>(1000 + i * 4096, uint8_t(i + 1)));
    proto::Room room;
    for (uint8_t i = 0; i < 3; ++i) {
        proto::Person p;
        p.id = i, p.look = i ? lookOf(came[i]) : proto::Look{};
        room.people.push_back(p);
    }
    {
        local::Wardrobe w("test");
        struct stat st;
        bool ok = stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700 && files(dir) == 0 && !w.Has(lookOf(came[1]));
        check("the folder for what comes is this user's alone, and empty", ok);
        w.Keep(lookOf(came[1]), came[1]);
        const std::string state = w.State(room, 0);
        const size_t at = state.find("{\"id\":1,\"file\":\"");
        const std::string file = at == std::string::npos ? "" : state.substr(at + 16, state.find('"', at + 16) - at - 16);
        ok = w.Has(lookOf(came[1])) && file.rfind(dir + "/", 0) == 0 && slurp(file) == came[1] && stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600 && files(dir) == 1;
        check("a file that came is kept there, whole, for this user only, and the room's row for its wearer says where", ok);
        check("...while someone whose file hasn't come has a row with none, and someone with no avatar has no row", state.find("{\"id\":2,\"file\":\"\"}") != std::string::npos && state.find("\"id\":0") == std::string::npos);
        w.Keep(lookOf(came[1]), came[1]), w.Keep(proto::Look{}, came[2]);
        for (uint8_t i = 2; i < 6; ++i) w.Keep(lookOf(came[i]), came[i]);
        ok = files(dir) == local::Wardrobe::kMostKept && w.Has(lookOf(came[4])) && !w.Has(lookOf(came[5]));
        check("so many files are kept at once and no more, each once", ok);
        w.Only({lookOf(came[2]), lookOf(came[5])});
        ok = files(dir) == 1 && w.Has(lookOf(came[2])) && !w.Has(lookOf(came[1])) && access(file.c_str(), F_OK) != 0;
        w.Keep(lookOf(came[5]), came[5]);
        check("a file nobody has on any more goes, and there is room again", ok && files(dir) == 2 && w.Has(lookOf(came[5])));
    }
    struct stat st;
    check("when the program ends, the files and their folder are gone", stat(dir.c_str(), &st) != 0);

    // what an earlier run left, and a folder that isn't ours
    mkdir(dir.c_str(), 0755);
    for (const char *name : {"/aa.ftmodel", "/bb.ftmodel.part", "/notes.txt"}) close(open((dir + name).c_str(), O_WRONLY | O_CREAT, 0600));
    {
        local::Wardrobe w("test");
        bool ok = stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700 && files(dir) == 1;
        w.Keep(lookOf(came[0]), came[0]);
        check("what an earlier run left behind is cleared out, the folder made private again, and it is used", ok && w.Has(lookOf(came[0])) && files(dir) == 2);
    }
    unlink((dir + "/notes.txt").c_str()), rmdir(dir.c_str());
    const std::string elsewhere = g_run + "/elsewhere";
    mkdir(elsewhere.c_str(), 0700);
    bool ok = symlink(elsewhere.c_str(), dir.c_str()) == 0;
    {
        local::Wardrobe w("test");
        w.Keep(lookOf(came[0]), came[0]);
        ok = ok && !w.Has(lookOf(came[0])) && files(elsewhere) == 0 && w.State(room, 0).find("{\"id\":1,\"file\":\"\"}") != std::string::npos;
    }
    check("a folder that is a link to somewhere else isn't used: nothing is kept", ok && lstat(dir.c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    unlink(dir.c_str());
    {
        local::Wardrobe odd("../test"), dot(".test"), none("");
        odd.Keep(lookOf(came[0]), came[0]), dot.Keep(lookOf(came[0]), came[0]), none.Keep(lookOf(came[0]), came[0]);
        ok = !odd.Has(lookOf(came[0])) && !dot.Has(lookOf(came[0])) && !none.Has(lookOf(came[0]));
    }
    unsetenv("XDG_RUNTIME_DIR");
    {
        local::Wardrobe w("test");
        w.Keep(lookOf(came[0]), came[0]);
        ok = ok && !w.Has(lookOf(came[0]));
    }
    check("a name that would lead out of the session's folder, or no session folder at all: nothing is kept", ok && files(g_run) == 1);
    rmdir(elsewhere.c_str());
}

int main() {
    char avatars[] = "/tmp/ft-wardrobe-avatars-XXXXXX", run[] = "/tmp/ft-wardrobe-run-XXXXXX";
    if (!mkdtemp(avatars) || !mkdtemp(run)) return std::perror("mkdtemp"), 1;
    g_avatars = avatars, g_run = run;
    setenv("FT_AVATARS_DIR", avatars, 1), setenv("XDG_RUNTIME_DIR", run, 1);
    wearing();
    keeping();
    for (const char *name : {"secret", "junk", "huge", "most"}) unlink(path(name).c_str());
    rmdir(path("folder").c_str()), rmdir(avatars), rmdir(run);
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed != 0;
}
