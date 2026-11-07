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

int main() {
    // Line ends: Windows' CRLF and the desktop's LF are the same text.
    CHECK(Normalize("a\r\nb\r\n") == "a\nb\n");
    CHECK(Normalize("a\rb") == "a\rb");  // a lone CR stays
    CHECK(Normalize("\r") == "\r");
    CHECK(Hash("a\r\nb") == Hash("a\nb"));
    CHECK(Hash("a") != Hash("b"));
    CHECK(Hash("") != 0);

    // Typing goes to the host: the desktop's text goes only when it changed since the last sync.
    CHECK(ShouldPush("hello", 0));
    CHECK(!ShouldPush("hello", Hash("hello")));
    CHECK(!ShouldPush("hello\r\n", Hash("hello\n")));
    CHECK(ShouldPush("hello again", Hash("hello")));
    CHECK(!ShouldPush("", 0));  // nothing that is text on the desktop
    CHECK(!ShouldPush(std::string(kMaxText + 1, 'x'), 0));
    CHECK(ShouldPush(std::string(kMaxText, 'x'), 0));

    // Typing leaves the host: its text comes over when it differs.
    CHECK(ShouldPull("from the pc", "from the frame"));
    CHECK(!ShouldPull("same\r\n", "same\n"));
    CHECK(!ShouldPull("", "frame text"));  // an image on the host: the desktop keeps its text
    CHECK(ShouldPull("pc", ""));
    CHECK(!ShouldPull(std::string(kMaxText + 1, 'x'), ""));

    // The sequence that mustn't lose an image copied on the host: the desktop's unchanged text
    // isn't sent back over it.
    {
        uint64_t last = 0;
        CHECK(ShouldPush("frame", last));
        last = Hash("frame");  // sent
        const std::string host = "";  // then an image was copied there
        CHECK(!ShouldPull(host, "frame"));
        CHECK(!ShouldPush("frame", last));  // back on the host: the image stays
        CHECK(ShouldPush("new frame text", last));  // until something new is copied on the desktop
    }

    // The host's answers.
    std::string why;
    CHECK(Classify(200, true, &why) == Answer::Ok);
    CHECK(Classify(401, false, &why) == Answer::Gone && why.find("Web UI") != std::string::npos);
    CHECK(Classify(404, false, &why) == Answer::Gone);
    CHECK(Classify(400, true, &why) == Answer::Gone);
    CHECK(Classify(403, true, &why) == Answer::Retry);
    CHECK(Classify(500, true, &why) == Answer::Gone && why.find("Linux") != std::string::npos);
    CHECK(Classify(500, false, &why) == Answer::Retry);
    CHECK(Classify(-1, false, &why) == Answer::Retry && why == "no answer from the host");
    CHECK(Classify(502, false, &why) == Answer::Retry && why == "the host answered 502");

    // The state file.
    char dir[] = "/tmp/clipboard-test-XXXXXX";
    if (!mkdtemp(dir)) return std::perror("mkdtemp"), 1;
    CHECK(StateFile(dir, "host-1") == std::string(dir) + "/frametop-clipboard-host-1");
    CHECK(StateFile(nullptr, "x") == "/tmp/frametop-clipboard-x");
    CHECK(StateFile("", "x") == "/tmp/frametop-clipboard-x");
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
