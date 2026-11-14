// The mark that lets a model be passed on (avatar/sharemark.h): a model gets it and is otherwise
// the file it was, one from a VRM never gets it here, and a file that isn't a whole model is
// refused and left as it is.
#include "../abstract/abstract.h"
#include "../bodyfile.h"
#include "../modelbytes.h"
#include "../sharemark.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static std::vector<uint8_t> bytes(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static void put(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
}

// Every chunk of two files but the one that says what the model is: the same, in the same order.
static bool sameBut(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    std::vector<modelfile::Chunk> one, two;
    std::string err;
    if (!modelfile::Chunks(a.data(), a.size(), one, err) || !modelfile::Chunks(b.data(), b.size(), two, err) || one.size() != two.size()) return false;
    for (size_t i = 0; i < one.size(); ++i) {
        if (one[i].tag != two[i].tag) return false;
        if (one[i].tag == modelbytes::Tag("INFO")) continue;
        if (one[i].size != two[i].size || !std::equal(one[i].data, one[i].data + one[i].size, two[i].data)) return false;
    }
    return true;
}

int main() {
    char folder[] = "/tmp/ft-sharemark-test-XXXXXX";
    if (!mkdtemp(folder)) std::perror("mkdtemp"), std::exit(2);
    const std::string dir = folder, path = dir + "/fox.ftmodel";
    modelfile::Model fox;
    fox.info = {"Fox", "mine", "someone", "gltf", false};
    const std::vector<uint8_t> was = modelfile::Write(fox, bodyfile::Write(abstract::Body(1)));
    modelfile::Info info;
    std::string err;

    put(path, was);
    const bool marked = sharemark::Allow(path, info, err);
    const std::vector<uint8_t> now = bytes(path);
    modelfile::Model read;
    bodyfile::Body body;
    bool hasBody = false;
    check("a model is marked as one that may be passed on", marked && info.mayShare && info.name == "Fox" && info.format == "gltf");
    check("...and reads so, with the name, licence and author it had",
          bodyfile::ReadFile(path, read, body, hasBody, err) && read.info.mayShare && read.info.name == "Fox" && read.info.licence == "mine" &&
              read.info.author == "someone" && read.info.format == "gltf");
    check("...its body as it was, byte for byte", hasBody && now != was && sameBut(was, now));
    check("marking it again changes nothing", sharemark::Allow(path, info, err) && info.mayShare && bytes(path) == now);

    for (const char *format : {"vrm 0", "vrm 1"}) {
        modelfile::Model vrm = fox;
        vrm.info.format = format;
        const std::vector<uint8_t> kept = modelfile::Write(vrm, bodyfile::Write(abstract::Body(1)));
        put(path, kept);
        err.clear();
        check("a model from a VRM whose licence said no stays unmarked: the licence decides", !sharemark::Allow(path, info, err) &&
                                                                                                  err.find("licence") != std::string::npos && bytes(path) == kept);
        vrm.info.mayShare = true;
        put(path, modelfile::Write(vrm, bodyfile::Write(abstract::Body(1))));
        check("...and one whose licence said yes is marked already", sharemark::Allow(path, info, err) && info.mayShare);
    }

    const std::vector<uint8_t> nothing = modelfile::Write(fox);
    put(path, nothing);
    err.clear();
    check("a file with no hands and no body is nothing to pass on", !sharemark::Allow(path, info, err) && err.find("no body") != std::string::npos && bytes(path) == nothing);

    // not a whole model: refused, and the file is left as it is
    bool refused = true;
    for (size_t cut = 0; cut < was.size(); cut += std::max<size_t>(1, was.size() / 257)) {
        const std::vector<uint8_t> part(was.begin(), was.begin() + long(cut));
        put(path, part);
        err.clear();
        refused = refused && !sharemark::Allow(path, info, err) && !err.empty() && bytes(path) == part;
    }
    check("a file cut short anywhere is refused, with why, and left as it is", refused);
    bool whole = true;
    for (size_t at = 0; at < was.size(); at += std::max<size_t>(1, was.size() / 509)) {
        std::vector<uint8_t> other = was;
        other[at] ^= 0x5a;
        put(path, other);
        err.clear();
        if (sharemark::Allow(path, info, err)) whole = whole && bodyfile::ReadFile(path, read, body, hasBody, err) && sameBut(other, bytes(path));
        else whole = whole && !err.empty() && bytes(path) == other;
    }
    check("...and one with a byte changed is refused and left, or is still a whole model with the same body", whole);
    put(path, {'n', 'o', 'p', 'e'});
    check("a file that is no model is refused", !sharemark::Allow(path, info, err) && bytes(path).size() == 4);
    unlink(path.c_str());
    check("...and so is one that isn't there, and nothing is made", !sharemark::Allow(path, info, err) && !err.empty() && access(path.c_str(), F_OK) != 0);

    rmdir(folder);
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
