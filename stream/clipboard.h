// The shared clipboard's rules (docs/shared-clipboard.md): when ft-stream
// sends the Frametop desktop's clipboard to a host, when it brings the host's back, and what
// the host's answers mean. Header-only so stream/test/clipboard-test.cpp checks it offline; the
// requests are host.cpp's (Client::Clipboard), the desktop's side is ft-clip.
//
// With "clipboard": true on its host in the layout, a stream syncs at the two moments typing
// changes sides: when typing goes to its panel ("focus" from ft-screens, or a key after a
// blur) the desktop's clipboard goes to the host, and when typing leaves ("blur") the host's
// comes to the desktop. What goes is a Content: the clipboard's text (up to kMaxText bytes),
// or when it holds no text, its PNG image (up to kMaxImage bytes). A host that takes text only
// (Vibepollo before images, or Apollo) answers 400 to an image, and its stream shares text only.
//
// Each host keeps one small state file, $XDG_RUNTIME_DIR/frametop-clipboard-ID (0600), shared
// by its streams: a hash of the content as it stood at the last sync. The desktop's goes to
// the host only when it has changed since then, so something copied on the host that can't
// come over (a file, a big image) isn't replaced by the Frame's older content on the way back.
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace clipboard {

constexpr size_t kMaxText = 1 << 20;  // ft-clip's FT_CLIP_MAX
constexpr size_t kMaxImage = 32 << 20;  // ft-clip's FT_CLIP_MAX_IMAGE (Vibepollo takes 64 MiB)

// What a clipboard holds for sharing: its text, or a PNG image's bytes. Empty data: nothing
// that can be shared.
struct Content {
    bool image = false;
    std::string data;
};

inline bool IsPng(const std::string &data) { return data.size() > 8 && data.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0; }

// Windows hands its text over with CRLF line ends and takes LF ones back as CRLF: the two
// count as the same text, and the desktop gets LF.
inline std::string Normalize(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i)
        if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')) out += text[i];
    return out;
}

// FNV-1a over the normalized text, or over "image" and the image's bytes, never 0 (0: no sync
// yet). A text's hash is the same as before images were shared, so a state file still holds.
inline uint64_t Hash(const Content &c) {
    uint64_t h = 14695981039346656037ull;
    if (c.image) {
        for (const unsigned char ch : std::string("image\n")) h = (h ^ ch) * 1099511628211ull;
        for (const unsigned char ch : c.data) h = (h ^ ch) * 1099511628211ull;
    } else {
        for (const unsigned char ch : Normalize(c.data)) h = (h ^ ch) * 1099511628211ull;
    }
    return h ? h : 1;
}
inline uint64_t Hash(const std::string &text) { return Hash(Content{false, text}); }

inline bool Fits(const Content &c) {
    return c.image ? IsPng(c.data) && c.data.size() <= kMaxImage : !c.data.empty() && c.data.size() <= kMaxText;
}

inline bool Same(const Content &a, const Content &b) {
    return a.image == b.image && (a.image ? a.data == b.data : Normalize(a.data) == Normalize(b.data));
}

// Typing went to the host: send the desktop's content if it changed since the last sync.
inline bool ShouldPush(const Content &desktop, uint64_t last) { return Fits(desktop) && Hash(desktop) != last; }

// Typing left the host: bring its content to the desktop if the two differ. Empty: nothing
// that can come over was copied there (a file, or an image a text-only host keeps), and the
// desktop keeps what it has.
inline bool ShouldPull(const Content &host, const Content &desktop) { return Fits(host) && !Same(host, desktop); }

// What a host's answer to /actions/clipboard means. Ok, or why this stream stops asking (Gone),
// stops asking for images (TextOnly), or just this time (Retry). Vibepollo answers 401 to a
// client not allowed the clipboard, 403 while the client isn't streaming, 400 to a type it
// doesn't share (an image, before Frametop's 2026-11 Vibepollo), and 500 when it can't set the
// clipboard: another app has the Windows clipboard open for a moment, or a Linux desktop
// without a clipboard for other programs (GNOME), or a Linux Vibepollo from before then, which
// had none. Sunshine has no such request (404). Below 0, Client::Clipboard's own: kNoAnswer,
// kTooBig, kNoPin.
enum class Answer { Ok, Retry, Gone, TextOnly };
constexpr long kNoAnswer = -1, kTooBig = -2, kNoPin = -3;
inline Answer Classify(long code, bool set, bool image, std::string *why) {
    switch (code) {
        case 200:
            return Answer::Ok;
        case 401:
            *why = "this display may not use the host's clipboard: allow Clipboard (both kinds) for its client in "
                   "Vibepollo's Web UI, Clients, or pair the display again";
            return Answer::Gone;
        case 400:
            if (image) {
                *why = "the host shares text only (update Vibepollo on it for images)";
                return Answer::TextOnly;
            }
            [[fallthrough]];
        case 404:
            *why = "the host has no shared clipboard (Frametop's Vibepollo has one)";
            return Answer::Gone;
        case 403:
            *why = "the host says this display isn't streaming yet";
            return Answer::Retry;
        case 500:
            *why = set ? std::string("the host couldn't take the ") + (image ? "image" : "text") +
                             " (another app had its clipboard open, or on Linux: no desktop session, a "
                             "desktop without a clipboard for other programs, or an older Vibepollo)"
                       : "the host couldn't read its clipboard";
            return Answer::Retry;
        case 413:
            *why = "the image is too big for the host";
            return Answer::Retry;
        case kTooBig:
            *why = image ? "the host's image is over 32 MiB, so it stays there"
                         : "the host's text is over 1 MiB, so it stays there";
            return Answer::Retry;
        case kNoPin:
            *why = "no saved certificate for this host, so its clipboard isn't shared (add it again in "
                   "Frametop Settings, Remote displays, which saves one when it signs in to its Web UI)";
            return Answer::Gone;
        default:
            *why = code < 0 ? "no answer from the host" : "the host answered " + std::to_string(code);
            return Answer::Retry;
    }
}

// "" without a runtime folder: the clipboard isn't shared then (a shared /tmp is no place
// for it, or for the desktop's socket).
inline std::string StateFile(const char *runtimeDir, const std::string &id) {
    return runtimeDir && *runtimeDir ? std::string(runtimeDir) + "/frametop-clipboard-" + id : "";
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
