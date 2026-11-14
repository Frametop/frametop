// The avatars' files on this Frame (wardrobe.h).
#include "wardrobe.h"

#include "../../avatar/modelfile.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace local {
namespace {

const char kKept[] = ".ftmodel", kPart[] = ".part";

bool EndsWith(const std::string &s, const char *end) {
    const std::string e = end;
    return s.size() > e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

// A folder that is this user's alone, with nothing of an earlier run left in it. "" when there
// can't be one.
std::string OwnFolder(const std::string &name) {
    const char *run = std::getenv("XDG_RUNTIME_DIR");
    if (!run || *run != '/' || name.empty() || name[0] == '.' || name.find('/') != std::string::npos) return "";
    const std::string dir = std::string(run) + "/" + name + "-looks";
    struct stat st;
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return "";
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || chmod(dir.c_str(), 0700) != 0) return "";
    DIR *d = opendir(dir.c_str());
    if (!d) return "";
    while (const dirent *e = readdir(d)) {
        const std::string file = e->d_name;
        if (EndsWith(file, kKept) || EndsWith(file, kPart)) unlink((dir + "/" + file).c_str());
    }
    closedir(d);
    return dir;
}

// Text as JSON has it. What isn't plain is left out.
std::string Quoted(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (uint8_t(c) < 0x20 || c == 0x7f) continue;
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}

bool WriteAll(int fd, const std::vector<uint8_t> &bytes) {
    for (size_t at = 0; at < bytes.size();) {
        const ssize_t n = write(fd, bytes.data() + at, bytes.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        at += size_t(n);
    }
    return true;
}

}  // namespace

Wardrobe::Wardrobe(const std::string &name) : dir_(OwnFolder(name)) {}

Wardrobe::~Wardrobe() {
    Only({});
    if (!dir_.empty()) rmdir(dir_.c_str());
}

std::string Wardrobe::Path(const proto::Look &look) const {
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : look.hash) hex += kHex[b >> 4], hex += kHex[b & 15];
    return dir_ + "/" + hex + kKept;
}

std::string Wardrobe::Wear(const std::string &name, proto::Look &look) {
    look = proto::Look{};
    struct stat st = {};
    const std::string path = modelfile::Folder() + "/" + name + ".ftmodel";
    const bool there = modelfile::NameOk(name) && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    const uint64_t stamp[4] = {uint64_t(st.st_dev), uint64_t(st.st_ino), uint64_t(st.st_size), uint64_t(st.st_mtim.tv_sec) * 1000000000u + uint64_t(st.st_mtim.tv_nsec)};
    if (there && name == name_ && std::equal(stamp, stamp + 4, stamp_)) return look = wornLook_, why_;   // as it was read last time
    name_ = name, why_.clear(), wornLook_ = proto::Look{}, worn_.reset();
    std::copy(stamp, stamp + 4, stamp_);
    if (name.empty()) return "";
    if (!there) return name_.clear(), why_ = "there is no avatar of that name";
    std::vector<uint8_t> bytes;
    modelfile::Model model;
    std::string err;
    if (uint64_t(st.st_size) > proto::kMostLook) return why_ = "that avatar's file is too large to send";
    if (!modelfile::ReadBytes(path, bytes, err) || !modelfile::Read(bytes.data(), bytes.size(), model, err)) return why_ = "that avatar's file can't be read: " + err;
    if (!model.info.mayShare) return why_ = "that avatar isn't marked as one the others may have";
    wornLook_ = flow::LookOf(bytes.data(), bytes.size());
    if (wornLook_.none()) return why_ = "that avatar's file is too large to send";
    worn_ = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    return look = wornLook_, "";
}

flow::FileBytes Wardrobe::Worn(const proto::Look &look) { return worn_ && proto::SameLook(look, wornLook_) ? worn_ : nullptr; }

bool Wardrobe::Has(const proto::Look &look) const {
    for (const proto::Look &k : kept_)
        if (proto::SameLook(k, look)) return true;
    return false;
}

void Wardrobe::Keep(const proto::Look &look, const std::vector<uint8_t> &bytes) {
    if (dir_.empty() || look.none() || Has(look) || kept_.size() >= kMostKept) return;
    const std::string path = Path(look), part = path + kPart;
    unlink(part.c_str());
    const int fd = open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const bool written = WriteAll(fd, bytes);
    // renamed into place, so the program that draws it never reads half a file
    if (close(fd) != 0 || !written || rename(part.c_str(), path.c_str()) != 0) {
        unlink(part.c_str());
        return;
    }
    kept_.push_back(look);
}

void Wardrobe::Only(const std::vector<proto::Look> &worn) {
    for (size_t i = kept_.size(); i-- > 0;) {
        bool still = false;
        for (const proto::Look &l : worn) still = still || proto::SameLook(l, kept_[i]);
        if (still) continue;
        unlink(Path(kept_[i]).c_str());
        kept_.erase(kept_.begin() + long(i));
    }
}

std::string Wardrobe::State(const proto::Room &room, int me) const {
    std::string out = "{\"wear\":" + Quoted(name_) + ",\"sent\":" + (worn_ ? "true" : "false") + ",\"why\":" + Quoted(why_) + ",\"people\":[";
    bool first = true;
    for (const proto::Person &p : room.people) {
        if (p.id == me || p.look.none()) continue;
        out += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(p.id) + ",\"file\":" + Quoted(Has(p.look) ? Path(p.look) : "") + "}";
        first = false;
    }
    return out + "]}";
}

}  // namespace local
