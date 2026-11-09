// stream/clipboard.h: the shared clipboard's rules, offline.
#include "clipboard.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failed = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c), ++g_failed; \
    } while (0)

using namespace clipboard;

static Content T(const std::string &text) { return {false, text}; }
static Content I(const std::string &png) { return {true, png}; }

int main() {
    // Line ends: Windows' CRLF and the desktop's LF are the same text.
    CHECK(Normalize("a\r\nb\r\n") == "a\nb\n");
    CHECK(Normalize("a\rb") == "a\rb");  // a lone CR stays
    CHECK(Normalize("\r") == "\r");
    CHECK(Hash("a\r\nb") == Hash("a\nb"));
    CHECK(Hash("a") != Hash("b"));
    CHECK(Hash("") != 0);

    // Typing goes to the host: the desktop's text goes only when it changed since the last sync.
    CHECK(ShouldPush(T("hello"), 0));
    CHECK(!ShouldPush(T("hello"), Hash("hello")));
    CHECK(!ShouldPush(T("hello\r\n"), Hash("hello\n")));
    CHECK(ShouldPush(T("hello again"), Hash("hello")));
    CHECK(!ShouldPush(T(""), 0));  // nothing on the desktop
    CHECK(!ShouldPush(T(std::string(kMaxText + 1, 'x')), 0));
    CHECK(ShouldPush(T(std::string(kMaxText, 'x')), 0));

    // Typing leaves the host: its text comes over when it differs.
    CHECK(ShouldPull(T("from the pc"), T("from the frame")));
    CHECK(!ShouldPull(T("same\r\n"), T("same\n")));
    CHECK(!ShouldPull(T(""), T("frame text")));  // a file on the host: the desktop keeps its text
    CHECK(ShouldPull(T("pc"), T("")));
    CHECK(!ShouldPull(T(std::string(kMaxText + 1, 'x')), T("")));

    // Images: PNG only, up to kMaxImage, and never the same hash as text of the same bytes.
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + "IHDR...";
    CHECK(IsPng(png) && !IsPng(png.substr(0, 8)) && !IsPng("GIF89a....."));
    CHECK(Fits(I(png)) && !Fits(I("GIF89a.....")) && !Fits(I("")));
    CHECK(Fits(I(png + std::string(kMaxImage - png.size(), 'x'))));
    CHECK(!Fits(I(png + std::string(kMaxImage - png.size() + 1, 'x'))));
    CHECK(Hash(I(png)) != Hash(T(png)) && Hash(I(png)) != 0);
    CHECK(Hash(T("a\r\nb")) == Hash("a\nb"));  // text hashes as before images
    CHECK(Hash(I(png + "\r\n")) != Hash(I(png + "\n")));  // an image's bytes are its own
    CHECK(ShouldPush(I(png), Hash("text")));
    CHECK(!ShouldPush(I(png), Hash(I(png))));
    CHECK(ShouldPull(I(png), T("frame text")));  // an image copied on the host comes over
    CHECK(ShouldPull(I(png), I(png + "2")));
    CHECK(!ShouldPull(I(png), I(png)));
    CHECK(ShouldPull(T("pc"), I(png)));
    CHECK(!ShouldPull(I("not a png"), T("")));

    // The sequence that mustn't lose what was copied on the host: the desktop's unchanged text
    // isn't sent back over it, whether it came over (an image) or not (a file).
    {
        uint64_t last = 0;
        CHECK(ShouldPush(T("frame"), last));
        last = Hash("frame");  // sent
        CHECK(!ShouldPull(T(""), T("frame")));  // then a file was copied there
        CHECK(!ShouldPush(T("frame"), last));  // back on the host: the file stays
        CHECK(ShouldPull(I(png), T("frame")));  // then an image, which comes over
        last = Hash(I(png));
        CHECK(!ShouldPush(I(png), last));  // and isn't sent back
        CHECK(ShouldPush(T("new frame text"), last));  // until something new is copied on the desktop
    }

    // The host's answers.
    std::string why;
    CHECK(Classify(200, true, false, &why) == Answer::Ok);
    CHECK(Classify(401, false, false, &why) == Answer::Gone && why.find("Web UI") != std::string::npos);
    CHECK(Classify(404, false, false, &why) == Answer::Gone);
    CHECK(Classify(400, true, false, &why) == Answer::Gone);
    // A host from before images: text still shared.
    CHECK(Classify(400, false, true, &why) == Answer::TextOnly && why.find("text only") != std::string::npos);
    CHECK(Classify(400, true, true, &why) == Answer::TextOnly);
    CHECK(Classify(403, true, false, &why) == Answer::Retry);
    // A Windows host's clipboard held open by another app for a moment: next time it works.
    CHECK(Classify(500, true, false, &why) == Answer::Retry && why.find("another app") != std::string::npos);
    CHECK(Classify(500, true, true, &why) == Answer::Retry && why.find("image") != std::string::npos);
    CHECK(Classify(500, false, false, &why) == Answer::Retry);
    CHECK(Classify(413, true, true, &why) == Answer::Retry);
    CHECK(Classify(kNoAnswer, false, false, &why) == Answer::Retry && why == "no answer from the host");
    CHECK(Classify(kTooBig, false, false, &why) == Answer::Retry && why.find("1 MiB") != std::string::npos);
    CHECK(Classify(kTooBig, false, true, &why) == Answer::Retry && why.find("32 MiB") != std::string::npos);
    CHECK(Classify(kNoPin, true, false, &why) == Answer::Gone && why.find("certificate") != std::string::npos);
    CHECK(Classify(502, false, false, &why) == Answer::Retry && why == "the host answered 502");

    // The state file.
    char dir[] = "/tmp/clipboard-test-XXXXXX";
    if (!mkdtemp(dir)) return std::perror("mkdtemp"), 1;
    CHECK(StateFile(dir, "host-1") == std::string(dir) + "/frametop-clipboard-host-1");
    CHECK(StateFile(nullptr, "x").empty());  // no runtime folder: not shared, never in /tmp
    CHECK(StateFile("", "x").empty());
    const std::string path = StateFile(dir, "h");
    CHECK(ReadLast(path) == 0);  // none yet
    CHECK(WriteLast(path, Hash("text")));
    CHECK(ReadLast(path) == Hash("text"));
    struct stat st{};
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(WriteLast(path, 0x1234));
    CHECK(ReadLast(path) == 0x1234);
    {
        FILE *f = std::fopen(path.c_str(), "w");
        std::fputs("not a hash", f);
        std::fclose(f);
        CHECK(ReadLast(path) == 0);
        f = std::fopen(path.c_str(), "w");
        std::fputs("00000000000000ff trailing", f);
        std::fclose(f);
        CHECK(ReadLast(path) == 0);
    }
    const std::string link = StateFile(dir, "link");
    CHECK(symlink(path.c_str(), link.c_str()) == 0);
    CHECK(ReadLast(link) == 0);  // a symlink isn't followed
    CHECK(!WriteLast(StateFile(dir, "no/such/dir"), 1));
    unlink(link.c_str());
    unlink(path.c_str());
    rmdir(dir);

    if (g_failed) return std::fprintf(stderr, "%d failed\n", g_failed), 1;
    std::printf("clipboard: all checks passed\n");
    return 0;
}
