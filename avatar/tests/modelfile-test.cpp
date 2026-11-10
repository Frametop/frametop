// Frametop's model file (avatar/modelfile.h): what's written reads back the same, and a file that
// breaks any rule of the format is refused whole. The reader is the one place a stranger's model
// is parsed, so the second half feeds it damaged files: each rule broken on its own, every
// truncation, and every byte changed.
#include "../modelfile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// A small hand that keeps every rule: a strip of triangles, each vertex on two joints.
static modelfile::Hand hand(int verts, int seed) {
    modelfile::Hand h;
    for (int v = 0; v < verts; ++v) {
        for (int k = 0; k < 3; ++k) {
            h.pos.push_back(int16_t((v * 37 + k * 11 + seed) % 2000 - 1000));
            h.nrm.push_back(int8_t(k == 1 ? 127 : 0));
        }
        const uint8_t a = uint8_t((v + seed) % modelfile::kHandJoints), b = uint8_t((v + seed + 1) % modelfile::kHandJoints);
        h.joints.insert(h.joints.end(), {a, b, 0, 0});
        h.weights.insert(h.weights.end(), {200, 55, 0, 0});
    }
    for (int v = 0; v + 2 < verts; ++v) h.idx.insert(h.idx.end(), {uint16_t(v), uint16_t(v + 1), uint16_t(v + 2)});
    for (int j = 0; j < modelfile::kHandJoints; ++j)
        for (int k = 0; k < 3; ++k) h.bind[j][k] = 0.004f * float(j) - 0.01f * float(k) + 0.001f * float(seed);
    return h;
}

static modelfile::Model model() {
    modelfile::Model m;
    m.info = {"Test hands", "CC0-1.0", "someone", "vrm", true};
    m.hasHand[0] = m.hasHand[1] = true;
    m.hands[0] = hand(7, 1);    // odd counts, so every array needs padding
    m.hands[1] = hand(10, 2);
    return m;
}

static bool same(const modelfile::Hand &a, const modelfile::Hand &b) {
    return a.pos == b.pos && a.nrm == b.nrm && a.joints == b.joints && a.weights == b.weights && a.idx == b.idx &&
           !std::memcmp(a.bind, b.bind, sizeof a.bind);
}

static bool reads(const std::vector<uint8_t> &bytes, std::string *why = nullptr) {
    modelfile::Model m;
    std::string err;
    const bool ok = modelfile::Read(bytes.data(), bytes.size(), m, err);
    if (why) *why = err;
    return ok;
}

// The file with one of a hand's rules broken: the writer doesn't check, so it writes it as it is.
template <typename F> static bool refused(F &&damage, const char *expect) {
    modelfile::Model m = model();
    damage(m);
    std::string why;
    const bool ok = reads(modelfile::Write(m), &why);
    if (ok || why.find(expect) == std::string::npos) std::printf("      got: %s\n", ok ? "(read)" : why.c_str());
    return !ok && why.find(expect) != std::string::npos;
}

static void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
    for (int k = 0; k < 4; ++k) b[at + k] = uint8_t(v >> (8 * k));
}
static void add32(std::vector<uint8_t> &b, uint32_t v) {
    b.resize(b.size() + 4);
    put32(b, b.size() - 4, v);
}
// Where a chunk starts (its tag), by its place among the file's chunks.
static size_t chunk(const std::vector<uint8_t> &b, int n) {
    size_t at = 8;
    for (; n > 0; --n) at += 8 + ((size_t(b[at + 4]) | size_t(b[at + 5]) << 8 | size_t(b[at + 6]) << 16 | size_t(b[at + 7]) << 24) + 3) / 4 * 4;
    return at;
}

int main() {
    const modelfile::Model m = model();
    const std::vector<uint8_t> bytes = modelfile::Write(m);
    {
        modelfile::Model r;
        std::string err;
        check("a written model reads back", modelfile::Read(bytes.data(), bytes.size(), r, err));
        check("its facts are the same", r.info.name == "Test hands" && r.info.licence == "CC0-1.0" && r.info.author == "someone" &&
                                            r.info.format == "vrm" && r.info.mayShare);
        check("its hands are the same", r.hasHand[0] && r.hasHand[1] && same(r.hands[0], m.hands[0]) && same(r.hands[1], m.hands[1]));
        check("and it writes the same bytes again", modelfile::Write(r) == bytes);
        check("the file is a whole number of words", bytes.size() % 4 == 0);
    }
    {
        modelfile::Model one = model(), r;
        one.hasHand[0] = false;
        one.info = {};
        const std::vector<uint8_t> b = modelfile::Write(one);
        std::string err;
        check("a model with one hand and no facts reads", modelfile::Read(b.data(), b.size(), r, err) && !r.hasHand[0] && r.hasHand[1] &&
                                                               r.info.name.empty() && !r.info.mayShare);
    }
    {
        // a chunk from a later Frametop, between the ones this one knows
        std::vector<uint8_t> b(bytes.begin(), bytes.begin() + long(chunk(bytes, 1)));
        b.insert(b.end(), {'B', 'O', 'D', 'Y'});
        add32(b, 5);
        b.insert(b.end(), {1, 2, 3, 4, 5, 0, 0, 0});
        b.insert(b.end(), bytes.begin() + long(chunk(bytes, 1)), bytes.end());
        modelfile::Model r;
        std::string err;
        check("a chunk it doesn't know is skipped", modelfile::Read(b.data(), b.size(), r, err) && same(r.hands[1], m.hands[1]));
    }
    {
        const std::string path = "build/tests/modelfile-test.ftmodel";
        modelfile::Model r;
        std::string err;
        check("a model written to a file reads back", modelfile::WriteFile(path, m, err) && modelfile::ReadFile(path, r, err) &&
                                                          same(r.hands[0], m.hands[0]));
        check("a file that isn't there is refused", !modelfile::ReadFile(path + ".none", r, err) && !err.empty());
        check("a folder that isn't there can't be written to", !modelfile::WriteFile("build/tests/none/x.ftmodel", m, err));
    }

    // each rule, broken on its own
    check("a joint a hand doesn't have", refused([](modelfile::Model &x) { x.hands[0].joints[5] = modelfile::kHandJoints; }, "joint"));
    check("weights that don't add up", refused([](modelfile::Model &x) { x.hands[1].weights[8] = 201; }, "add up"));
    check("a triangle past the vertices", refused([](modelfile::Model &x) { x.hands[1].idx[4] = 10; }, "triangle"));
    check("a joint that isn't a number", refused([](modelfile::Model &x) { x.hands[0].bind[3][1] = 0.0f / 0.0f; }, "out of range"));
    check("a joint a metre away", refused([](modelfile::Model &x) { x.hands[0].bind[24][2] = 1.0f; }, "out of range"));
    check("no triangles", refused([](modelfile::Model &x) { x.hands[0].idx.clear(); }, "too few"));
    check("two vertices", refused([](modelfile::Model &x) { x.hands[0] = hand(2, 0); }, "too few"));
    check("too many vertices", refused([](modelfile::Model &x) { x.hands[0] = hand(modelfile::kMaxHandVerts + 1, 0); }, "too many"));
    {   // the writer takes control characters out of text, so one is put into the written bytes
        std::vector<uint8_t> b = modelfile::Write(model());
        const std::string name = model().info.name;
        const auto at = std::search(b.begin(), b.end(), name.begin(), name.end());
        std::string why;
        if (at != b.end()) *at = 0x1b;
        check("a control character in a name", at != b.end() && !reads(b, &why) && why.find("plain") != std::string::npos);
    }
    {
        modelfile::Model none = model();
        none.hasHand[0] = none.hasHand[1] = false;
        std::string why;
        check("no hands at all", !reads(modelfile::Write(none), &why) && why.find("nothing") != std::string::npos);
    }
    {
        std::string why;
        std::vector<uint8_t> b = bytes;
        b[0] = 'X';
        check("another file's magic", !reads(b, &why) && why.find("not a Frametop model") != std::string::npos);
        b = bytes;
        put32(b, 4, 2);
        check("a later version", !reads(b, &why) && why.find("version") != std::string::npos);
        b = bytes;
        const size_t left = chunk(bytes, 1), right = chunk(bytes, 2);
        b.insert(b.end(), bytes.begin() + long(left), bytes.begin() + long(right));
        check("two left hands", !reads(b, &why) && why.find("two hands") != std::string::npos);
        b = bytes;
        b.insert(b.end(), bytes.begin() + 8, bytes.begin() + long(left));
        check("two INFO chunks", !reads(b, &why) && why.find("two INFO") != std::string::npos);
        b = bytes;
        put32(b, left + 4, 0xfffffff0u);
        check("a chunk longer than the file", !reads(b, &why) && why.find("past") != std::string::npos);
        b = bytes;
        put32(b, left + 8 + 4, 0x40000000u);   // a billion vertices: refused before anything is sized for them
        check("a count far past the limit", !reads(b, &why) && why.find("too many") != std::string::npos);
        b = bytes;
        put32(b, left + 8 + 4, 8);   // one more vertex than the chunk holds
        check("a count past the chunk's end", !reads(b, &why) && why.find("past") != std::string::npos);
        b = bytes;
        put32(b, left + 8 + 4, 6);   // one fewer: bytes left over
        check("a count short of the chunk's end", !reads(b, &why));
        b = bytes;
        b[left + 8 + 1] = 1;
        check("a header byte that should be zero", !reads(b, &why) && why.find("header") != std::string::npos);
        b = bytes;
        b[left + 8] = 2;
        check("a third side", !reads(b, &why) && why.find("header") != std::string::npos);
        b.assign(8, 0);
        std::memcpy(b.data(), "FTMD", 4);
        put32(b, 4, 1);
        for (int k = 0; k < 65; ++k) b.insert(b.end(), {'N', 'O', 'N', 'E', 0, 0, 0, 0});
        check("too many chunks", !reads(b, &why) && why.find("too many chunks") != std::string::npos);
    }

    // every truncation is refused, and none reads past its end (run under the address sanitizer)
    {
        int read = 0;
        const size_t ends[] = {chunk(bytes, 1), chunk(bytes, 2)};   // whole files with fewer chunks
        for (size_t n = 0; n < bytes.size(); ++n) {
            const std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + long(n));
            if (reads(cut) && n != ends[1]) ++read;
        }
        check("every cut-off file is refused (but the one that ends after a whole hand)", read == 0);
        check("and that one has its left hand only", reads(std::vector<uint8_t>(bytes.begin(), bytes.begin() + long(ends[1]))) &&
                                                         !reads(std::vector<uint8_t>(bytes.begin(), bytes.begin() + long(ends[0]))));
    }
    // every byte changed: read or refused, and whatever reads keeps the rules
    {
        int broke = 0, took = 0;
        for (size_t at = 0; at < bytes.size(); ++at) {
            for (const uint8_t flip : {uint8_t(0x01), uint8_t(0x80), uint8_t(0xff)}) {
                std::vector<uint8_t> b = bytes;
                b[at] ^= flip;
                modelfile::Model r;
                std::string err, why;
                if (!modelfile::Read(b.data(), b.size(), r, err)) continue;
                ++took;
                for (int side = 0; side < 2; ++side)
                    if (r.hasHand[side] && !modelfile::Valid(r.hands[side], why)) ++broke;
            }
        }
        std::printf("      %d changed files read (a position, a normal or a letter changed)\n", took);
        check("a changed file that reads still keeps the rules", broke == 0 && took > 0);
    }
    // names and text: a model's name is a file's name, so nothing gets in that could leave the folder
    {
        using modelfile::NameFrom;
        using modelfile::NameOk;
        check("a name is lower case letters, digits, - and _", NameOk("my-hands_2") && NameOk(std::string(31, 'a')));
        check("a name that could leave the folder isn't one", !NameOk("") && !NameOk("..") && !NameOk("a/b") && !NameOk("a.b") &&
                                                                  !NameOk("Hands") && !NameOk("a b") && !NameOk(std::string(32, 'a')) &&
                                                                  !NameOk(std::string("a\0b", 3)));
        check("any text makes a name", NameFrom("My Hands (v2)") == "my-hands-v2" && NameFrom("../../etc/passwd") == "etc-passwd" &&
                                           NameFrom("") == "model" && NameFrom("\xc3\x85\xc3\x84") == "model" && NameFrom("__x") == "__x");
        const std::string longest = NameFrom(std::string(30, 'a') + " b" + std::string(40, 'c'));
        check("a long one is cut, not on a dash", longest == std::string(30, 'a') && NameOk(NameFrom(std::string(200, 'Z'))));
        bool all = true;
        for (int c = 1; c < 256; ++c) all = all && NameOk(NameFrom(std::string("x") + char(c) + "y" + char(c)));
        check("whatever the bytes, the name made is one", all);

        using modelfile::Plain;
        check("text loses its control characters", Plain("a\n\tb\x7f\x1b[31mc") == "ab[31mc" && Plain("") == "");
        const std::string cut = Plain(std::string(95, 'a') + "\xc3\xa9\xc3\xa9");   // a 2-byte letter across the limit
        check("long text is cut between characters", cut == std::string(95, 'a') && Plain(std::string(300, 'b')).size() == modelfile::kMaxText);
        modelfile::Model m = model();
        m.info.name = "line one\nline two" + std::string(200, '!');
        const std::vector<uint8_t> out = modelfile::Write(m);
        std::string err;
        modelfile::Model back;
        check("what's written with such text reads back plain", modelfile::Read(out.data(), out.size(), back, err) &&
                                                                    back.info.name == Plain(m.info.name));

        setenv("FT_AVATARS_DIR", "/somewhere", 1);
        check("the folder is FT_AVATARS_DIR", modelfile::Folder() == "/somewhere");
        unsetenv("FT_AVATARS_DIR");
        setenv("XDG_DATA_HOME", "/data", 1);
        check("or in XDG_DATA_HOME", modelfile::Folder() == "/data/frametop/avatars");
        unsetenv("XDG_DATA_HOME");
        setenv("HOME", "/home/me", 1);
        check("or in your home's data folder", modelfile::Folder() == "/home/me/.local/share/frametop/avatars");
    }
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
