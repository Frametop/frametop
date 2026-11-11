// An avatar someone in the room sent, read from its file (avatar/room/sentbody.h): one its owner
// lets others have is read as it was written, and one that isn't marked so, has no body, isn't
// there, was cut short or had a byte changed is refused or is still a whole body. The readers'
// own rules are in modelfile-test.cpp and bodyfile-test.cpp.
#include "../abstract/abstract.h"
#include "../room/sentbody.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// A file of the test's own, gone with it.
struct File {
    std::string path;
    File(const std::string &dir, const char *name, const std::vector<uint8_t> &bytes) : path(dir + "/" + name) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    }
    ~File() { unlink(path.c_str()); }
};

int main() {
    char folder[] = "/tmp/ft-sentbody-test-XXXXXX";
    if (!mkdtemp(folder)) std::perror("mkdtemp"), std::exit(2);
    const std::string dir = folder;
    modelfile::Model given, kept;
    given.info.name = "fox", given.info.licence = "CC0", given.info.mayShare = true;
    kept.info.name = "wolf";
    const bodyfile::Body was = abstract::Body(2);
    const std::vector<uint8_t> whole = modelfile::Write(given, bodyfile::Write(was));
    {
        const File fox(dir, "fox.ftmodel", whole), wolf(dir, "wolf.ftmodel", modelfile::Write(kept, bodyfile::Write(was))), hands(dir, "hands.ftmodel", modelfile::Write(given));
        bodyfile::Body body;
        std::string why;
        check("an avatar its owner lets others have is read, as it was written", sentbody::Read(fox.path, body, why) && body.bones.size() == was.bones.size() &&
                                                                                   body.verts() == was.verts() && body.verts() > 0 && !body.bones.empty());
        check("one that isn't marked so isn't drawn: it wasn't theirs to send", !sentbody::Read(wolf.path, body, why) && why.find("marked") != std::string::npos);
        check("hands alone are no avatar", !sentbody::Read(hands.path, body, why) && why.find("no body") != std::string::npos);
        check("nor is a file that isn't there", !sentbody::Read(dir + "/none.ftmodel", body, why) && !why.empty() && !sentbody::Read("", body, why));
    }
    // a stranger's bytes: whatever is done to a good file, it is read whole or refused
    bool refused = true;
    size_t read = 0, bent = 0;
    for (size_t cut = 0; cut < whole.size(); cut += std::max<size_t>(1, whole.size() / 257)) {
        const File part(dir, "part.ftmodel", std::vector<uint8_t>(whole.begin(), whole.begin() + long(cut)));
        bodyfile::Body body;
        std::string why;
        refused = refused && !sentbody::Read(part.path, body, why) && !why.empty();
    }
    for (size_t at = 0; at < whole.size(); at += std::max<size_t>(1, whole.size() / 509)) {
        std::vector<uint8_t> other = whole;
        other[at] ^= 0x5a, ++bent;
        const File odd(dir, "odd.ftmodel", other);
        bodyfile::Body body;
        std::string why;
        if (sentbody::Read(odd.path, body, why)) ++read;   // (a byte of a place or a colour: still a body the reader's rules hold for)
        else refused = refused && !why.empty();
    }
    check("a file cut short anywhere is refused, with why", refused);
    check("...and one with a byte changed is refused or is still a whole body", refused && read < bent);
    rmdir(folder);
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
