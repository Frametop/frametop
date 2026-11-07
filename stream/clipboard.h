// The shared clipboard's rules (docs/shared-clipboard.md): when ft-stream
// sends the Frametop desktop's text to a host, when it brings the host's back, and what the
// host's answers mean. Header-only so stream/test/clipboard-test.cpp checks it offline; the
// requests are host.cpp's (Client::Clipboard), the desktop's side is ft-clip.
//
// With "clipboard": true on its host in the layout, a stream syncs at the two moments typing
// changes sides: when typing goes to its panel ("focus" from ft-screens, or a key after a
// blur) the desktop's text goes to the host, and when typing leaves ("blur") the host's text
// comes to the desktop. Text only, both ways, up to kMaxText bytes.
//
// Each host keeps one small state file, $XDG_RUNTIME_DIR/frametop-clipboard-ID (0600), shared
// by its streams: a hash of the text as it stood at the last sync. The desktop's text goes to
// the host only when it has changed since then, so something copied on the host that isn't
// text (an image, a file) isn't replaced by the Frame's older text on the way back in.
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace clipboard {

constexpr size_t kMaxText = 1 << 20;  // ft-clip's FT_CLIP_MAX

// Windows hands its text over with CRLF line ends and takes LF ones back as CRLF: the two
// count as the same text, and the desktop gets LF.
inline std::string Normalize(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
        if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')) out += text[i];
    return out;
}

// FNV-1a over the normalized text, never 0 (0: no sync yet).
inline uint64_t Hash(const std::string &text) {
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char c : Normalize(text)) h = (h ^ c) * 1099511628211ull;
    return h ? h : 1;
}

inline bool Fits(const std::string &text) { return !text.empty() && text.size() <= kMaxText; }

// Typing went to the host: send the desktop's text if it changed since the last sync.
inline bool ShouldPush(const std::string &desktop, uint64_t last) { return Fits(desktop) && Hash(desktop) != last; }

// Typing left the host: bring its text to the desktop if the two differ. Empty: nothing that
// is text was copied there (an image), and the desktop keeps what it has.
inline bool ShouldPull(const std::string &host, const std::string &desktop) {
    return Fits(host) && Normalize(host) != Normalize(desktop);
}

// What a host's answer to /actions/clipboard means. Ok, or why this stream stops asking (Gone)
// or just this time (Retry). Vibepollo (Apollo's clipboard, Windows) answers 401 to a client
// not allowed the clipboard, 403 while the client isn't streaming, and 500 when it can't set
// it (its Linux version has no clipboard yet). Sunshine has no such request (404).
enum class Answer { Ok, Retry, Gone };
inline Answer Classify(long code, bool set, std::string *why) {
    switch (code) {
        case 200:
            return Answer::Ok;
        case 401:
            *why = "this display may not use the host's clipboard: allow Clipboard (both kinds) for its client in "
                   "Vibepollo's Web UI, Clients, or pair the display again";
            return Answer::Gone;
        case 404:
        case 400:
            *why = "the host has no shared clipboard (Vibepollo on Windows has one)";
            return Answer::Gone;
        case 403:
            *why = "the host says this display isn't streaming yet";
            return Answer::Retry;
        case 500:
            *why = set ? "the host couldn't take the text (Vibepollo on Linux has no clipboard yet)"
                       : "the host couldn't read its clipboard";
            return set ? Answer::Gone : Answer::Retry;
        default:
            *why = code < 0 ? "no answer from the host" : "the host answered " + std::to_string(code);
            return Answer::Retry;
    }
}

inline std::string StateFile(const char *runtimeDir, const std::string &id) {
    return std::string(runtimeDir && *runtimeDir ? runtimeDir : "/tmp") + "/frametop-clipboard-" + id;
}

// The last sync's hash, 0 if none (no file, or not one of ours: not a plain file of this
// user's, or garbled).
inline uint64_t ReadLast(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st{};
    char buf[32] = {};
    const bool ours = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_size < 32;
    const ssize_t n = ours ? read(fd, buf, sizeof buf - 1) : -1;
    close(fd);
    if (n <= 0) return 0;
    unsigned long long h = 0;
    char extra = 0;
    return std::sscanf(buf, "%16llx%c", &h, &extra) >= 1 && (extra == 0 || extra == '\n') ? uint64_t(h) : 0;
}

// Written next to the file and renamed over it, so a stream reading it never sees half.
inline bool WriteLast(const std::string &path, uint64_t hash) {
    const std::string tmp = path + ".new";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    char buf[32];
    const int n = std::snprintf(buf, sizeof buf, "%016llx\n", (unsigned long long)hash);
    const bool ok = write(fd, buf, size_t(n)) == n;
    close(fd);
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace clipboard
