#include "screenfeed.h"

#include "words.h"

#include <cstdio>
#include <unistd.h>

namespace media {

void ScreenFeed::Say(const std::string &words) const { SendWords(fd_, words); }

void ScreenFeed::Done() {
    if (!pending_) return;
    pending_ = false;
    Say("done " + std::to_string(frame_));
}

bool ScreenFeed::Begin(const Pictures &pictures, std::string &err) {
    if (!blit_.Open(pictures, err)) return false;
    wanted_ = true, refused_ = 0;
    Say("want 1");
    return true;
}

void ScreenFeed::Fresh() {
    if (wanted_) Say("want 1");
}

void ScreenFeed::End() {
    if (wanted_) Say("want 0");
    wanted_ = false;
    Done();
    blit_.Close();
}

bool ScreenFeed::Serve() {
    for (;;) {
        std::string msg;
        int fds[kMostWordFds], nfds = 0;
        const int got = TakeWords(fd_, msg, fds, nfds);
        if (got == 0) return true;
        if (got < 0) return false;   // ft-screens closed its end
        const bool quit = msg == "quit";
        if (!quit) Message(msg.c_str(), fds, nfds);
        for (int k = 0; k < nfds; ++k) close(fds[k]);   // an import keeps the buffer itself
        if (quit) return false;
    }
}

void ScreenFeed::Message(const char *msg, const int *fds, int nfds) {
    unsigned id = 0;
    Dmabuf d;
    unsigned long long modifier = 0;
    int at = 0;
    if (std::sscanf(msg, "buffer %u %u %u %x %llx %d%n", &id, &d.width, &d.height, &d.format, &modifier, &d.planes, &at) == 6) {
        if (!wanted_ || d.planes < 1 || d.planes > 4 || d.planes != nfds) return;
        d.modifier = modifier;
        for (int i = 0; i < d.planes; ++i) {
            int more = 0;
            if (std::sscanf(msg + at, " %u %u%n", &d.offset[i], &d.stride[i], &more) != 2) return;
            at += more, d.fd[i] = fds[i];
        }
        std::string err;
        if (!blit_.Import(id, d, err) && refused_++ < 4) {
            char why[200];
            std::snprintf(why, sizeof why, "state can't read a %ux%u buffer (format %.4s, modifier 0x%llx): %s", d.width, d.height,
                          reinterpret_cast<const char *>(&d.format), modifier, err.c_str());
            Say(why);
            std::printf("%s\n", why + 6);
        }
    } else if (std::sscanf(msg, "frame %u", &id) == 1) {
        Done();   // (ft-screens sends one at a time: an older one it gave up on)
        pending_ = true, frame_ = id;
        if (!wanted_) Done();
    } else if (std::sscanf(msg, "forget %u", &id) == 1) {
        if (pending_ && frame_ == id) Done();
        blit_.Forget(id);
    }
}

bool ScreenFeed::Draw(Pictures &, int into, double) {
    if (!pending_) return false;
    const bool drawn = blit_.Draw(frame_, into);
    Done();
    return drawn;
}

void ScreenFeed::Skip(double) { Done(); }

}  // namespace media
