// Apple High Performance Screen Sharing client (MIT), part of apple_hp.h.
// The private credential store: ~/.local/share/frametop-stream/hosts/HOST.apple (0600), with
// the auth mode that worked at sign-in.
#pragma once

#include "apple_hp_base.h"

namespace ft_apple_hp {

enum class AuthMode { kAuto, kSrp, kNonSrp };

inline std::string AppleCredsDir() {
    const char *home = std::getenv("HOME");
    return std::string(home && *home ? home : "/tmp") + "/.local/share/frametop-stream/hosts";
}

inline std::string SanitizeCredHost(const std::string &host) {
    std::string out;
    out.reserve(host.size());
    for (char c : host) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_') {
            out.push_back(c);
        }
    }
    return out.empty() ? "host" : out;
}

inline std::string AppleCredsPath(const std::string &host, const std::string &base_dir = "") {
    const std::string dir = base_dir.empty() ? AppleCredsDir() : base_dir;
    return dir + "/" + SanitizeCredHost(host) + ".apple";
}

inline bool EnsurePrivateDir(const std::string &dir) {
    for (size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos == dir.size() || dir[pos] == '/') {
            const std::string sub = dir.substr(0, pos);
            if (!sub.empty() && mkdir(sub.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    chmod(dir.c_str(), 0700);
    return true;
}

// The file holds the user, the password and the auth mode that worked at sign-in
// ("srp" or "nonsrp"). Later connections use only that mode, so a spoofed
// server can't talk a SRP Mac's stored password down to the RSA path.
inline bool SaveAppleCreds(const std::string &path, const std::string &username, const std::string &password,
                           AuthMode mode = AuthMode::kSrp) {
    if (username.empty() || password.empty() || mode == AuthMode::kAuto) return false;
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && !EnsurePrivateDir(path.substr(0, slash))) return false;
    const std::string tmp = path + ".tmp." + std::to_string(getpid());
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = open(tmp.c_str(), flags, 0600);
    if (fd < 0) return false;
    fchmod(fd, 0600);
    const std::string mode_word = mode == AuthMode::kNonSrp ? "nonsrp" : "srp";
    std::string body = username + "\n" + password + "\n" + mode_word + "\n";
    const ssize_t n = write(fd, body.data(), body.size());
    OPENSSL_cleanse(body.data(), body.size());
    const int sync_rc = fsync(fd);
    close(fd);
    if (n != ssize_t(username.size() + password.size() + mode_word.size() + 3) || sync_rc != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

inline bool LoadAppleCreds(const std::string &path, std::string *username_out, std::string *password_out,
                           AuthMode *mode_out = nullptr) {
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = open(path.c_str(), flags);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0077) != 0 || st.st_size <= 0 || st.st_size > 4096) {
        close(fd);
        return false;
    }
    std::string buf(size_t(st.st_size), '\0');
    const ssize_t n = read(fd, buf.data(), buf.size());
    close(fd);
    if (n <= 0) {
        OPENSSL_cleanse(buf.data(), buf.size());
        return false;
    }
    buf.resize(size_t(n));
    const size_t nl1 = buf.find('\n');
    if (nl1 == std::string::npos) {
        OPENSSL_cleanse(buf.data(), buf.size());
        return false;
    }
    std::string u = buf.substr(0, nl1);
    if (!u.empty() && u.back() == '\r') u.pop_back();
    const size_t nl2 = buf.find('\n', nl1 + 1);
    std::string p = (nl2 == std::string::npos) ? buf.substr(nl1 + 1) : buf.substr(nl1 + 1, nl2 - (nl1 + 1));
    if (!p.empty() && p.back() == '\r') p.pop_back();
    // A file without a mode line counts as SRP: never fall back on our own.
    if (mode_out) {
        const size_t m0 = nl2 == std::string::npos ? buf.size() : nl2 + 1;
        const size_t m1 = buf.find('\n', m0);
        std::string m = buf.substr(m0, m1 == std::string::npos ? std::string::npos : m1 - m0);
        if (!m.empty() && m.back() == '\r') m.pop_back();
        *mode_out = m == "nonsrp" ? AuthMode::kNonSrp : AuthMode::kSrp;
    }
    OPENSSL_cleanse(buf.data(), buf.size());
    if (u.empty() || p.empty()) {
        OPENSSL_cleanse(p.data(), p.size());
        return false;
    }
    *username_out = std::move(u);
    *password_out = std::move(p);
    return true;
}

}  // namespace ft_apple_hp
