// track/npuserver.cpp: which peers count as ft-npud (root, this user, or the container's overflow uid at a path
// only they can write), and finding the daemon at /run/host inside the container.
//   build/npuserver-test
#include "../track/npuserver.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures; \
    } while (0)

int main() {
    const uid_t self = getuid(), over = 65534, other = self == 4242 ? 4343 : 4242;
    std::string why;

    // root and this user, at any path (the checks before this change)
    CHECK(npu_server_trusted(0, self, over, "/nowhere", why));
    CHECK(npu_server_trusted(self, self, over, "/nowhere", why));
    // anyone else mapped is refused, and says who
    CHECK(!npu_server_trusted(other, self, over, "/", why));
    CHECK(why.find(std::to_string(other)) != std::string::npos);

    // the overflow uid: only where the chain up to / is safe. A private directory chain this user owns:
    char tmpl[] = "/tmp/npuserver-XXXXXX";
    const std::string base = mkdtemp(tmpl);   // 0700, this user's
    const std::string safe = base + "/run", open_dir = base + "/open", deep = open_dir + "/ft-npud";
    CHECK(mkdir(safe.c_str(), 0755) == 0);
    CHECK(mkdir(open_dir.c_str(), 0755) == 0 && chmod(open_dir.c_str(), 0777) == 0);
    CHECK(mkdir(deep.c_str(), 0755) == 0);
    // /tmp itself is world-writable (sticky or not, others can add entries), so no chain under it is safe ...
    CHECK(!npu_server_trusted(over, self, over, safe, why));
    CHECK(why.find("others may write to /tmp") != std::string::npos);
    // ... nor one with a world-writable directory of its own further down
    CHECK(!npu_server_trusted(over, self, over, deep, why));
    CHECK(why.find(open_dir) != std::string::npos);
    // a group-writable directory too
    CHECK(chmod(open_dir.c_str(), 0775) == 0);
    CHECK(!npu_server_trusted(over, self, over, deep, why));
    // not a directory, or relative
    CHECK(!npu_server_trusted(over, self, over, base + "/missing", why));
    CHECK(!npu_server_trusted(over, self, over, "run/ft-npud", why));
    // / alone: root's (or the container's root), not writable by others on any sane system
    CHECK(npu_server_trusted(over, self, over, "/", why));
    // finding the daemon: DIR when it has the heads file (or it's relative or empty), else DIR's error later
    CHECK(npu_server_dir(base) == base);
    const std::string heads = safe + "/ft-npud.heads";
    if (FILE *f = std::fopen(heads.c_str(), "w")) std::fclose(f);
    CHECK(npu_server_dir(safe) == safe);
    unlink(heads.c_str());
    CHECK(npu_server_dir("relative/dir") == "relative/dir");
    CHECK(npu_server_dir("") == "");

    // In the container (/run/host is the host's /, owned by the overflow uid): the host's /run passes for the
    // overflow uid, a directory owned by a uid that is none of the three is refused, and the daemon's directory
    // is found under /run/host when it's running.
    struct stat host{};
    if (lstat("/run/host", &host) == 0 && host.st_uid == npu_overflow_uid()) {
        const uid_t ov = npu_overflow_uid();
        CHECK(npu_server_trusted(ov, self, ov, "/run/host/run", why));
        CHECK(!npu_server_trusted(12345, self, 12345, "/run/host/run", why));
        CHECK(why.find("belongs to uid " + std::to_string(ov)) != std::string::npos);
        struct stat heads{};
        if (lstat("/run/ft-npud/ft-npud.heads", &heads) != 0 && lstat("/run/host/run/ft-npud/ft-npud.heads", &heads) == 0)
            CHECK(npu_server_dir("/run/ft-npud") == "/run/host/run/ft-npud");
        std::printf("npuserver: container checks ran\n");
    }

    // HANDS_NPU: the daemon's own folder unless it's switched off or names another
    CHECK(npu_dir_setting("") == "/run/ft-npud" && npu_dir_setting("auto") == "/run/ft-npud" && npu_dir_setting("1") == "/run/ft-npud");
    CHECK(npu_dir_setting("0").empty() && npu_dir_setting("off").empty() && npu_dir_setting("cpu").empty());
    CHECK(npu_dir_setting("/run/other") == "/run/other");

    // overflowuid reads as a number
    CHECK(npu_overflow_uid() > 0);

    rmdir(deep.c_str()), rmdir(open_dir.c_str()), rmdir(safe.c_str()), rmdir(base.c_str());
    if (failures) return std::printf("npuserver: %d failures\n", failures), 1;
    std::printf("npuserver: ok\n");
    return 0;
}
