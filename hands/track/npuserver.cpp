#include "npuserver.h"

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool exists(const std::string &path) {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0;
}

}  // namespace

std::string npu_dir_setting(const std::string &value) {
    if (value.empty() || value == "auto" || value == "1") return kNpuDefaultDir;
    if (value == "0" || value == "off" || value == "cpu") return "";
    return value;
}

std::string npu_server_dir(const std::string &dir) {
    // the heads file: the daemon writes it next to its socket, and the session reads it first
    const std::string heads = "/ft-npud.heads";
    if (dir.empty() || dir[0] != '/' || exists(dir + heads)) return dir;
    const std::string host = "/run/host" + dir;
    return exists(host + heads) ? host : dir;
}

uid_t npu_overflow_uid() {
    unsigned long v = 65534;
    if (FILE *f = std::fopen("/proc/sys/kernel/overflowuid", "r")) {
        if (std::fscanf(f, "%lu", &v) != 1) v = 65534;
        std::fclose(f);
    }
    return uid_t(v);
}

bool npu_server_trusted(uid_t peer, uid_t self, uid_t overflow, const std::string &dir, std::string &why) {
    if (peer == 0 || peer == self) return true;
    if (peer != overflow) return why = "served by uid " + std::to_string(peer) + ", not root or this user", false;
    if (dir.empty() || dir[0] != '/') return why = "served by an unmapped uid at a relative path", false;
    // every directory from dir up to /, dir itself first
    for (std::string d = dir;;) {
        struct stat st{};
        if (::lstat(d.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return why = "served by an unmapped uid; " + d + " is not a directory", false;
        if (st.st_uid != 0 && st.st_uid != self && st.st_uid != overflow)
            return why = "served by an unmapped uid; " + d + " belongs to uid " + std::to_string(st.st_uid), false;
        if (st.st_mode & (S_IWGRP | S_IWOTH))
            return why = "served by an unmapped uid; others may write to " + d, false;
        if (d == "/") return true;
        const size_t slash = d.find_last_of('/');
        d = slash == 0 ? "/" : d.substr(0, slash);
    }
}
