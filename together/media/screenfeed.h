// A feed of one of ft-screens' screens (screens/share.c): the screen's buffers come over a
// SOCK_SEQPACKET socket as DMA-BUFs, each once, and then a word whenever one holds a new picture.
// The picture is drawn into the encoder's on the GPU (blit.h) and the buffer handed back, so
// KWin can draw into it again.
//
// From ft-screens, one message a packet:
//   buffer <id> <width> <height> <format hex> <modifier hex> <planes> <offset> <stride> ...
//                      a buffer of the screen's, its planes' fds with the message
//   frame <id>         that buffer holds the screen's newest picture; ft-screens keeps it as it
//                      is until "done", and sends no other frame until then
//   forget <id>        the buffer is gone
//   quit
// To ft-screens:
//   want 1 | want 0    pictures are wanted (someone watches the panel) or not. After "want 0"
//                      every buffer is forgotten here; after "want 1" ft-screens describes them
//                      again and sends the screen's picture as it is now. "want 1" again: the
//                      picture as it is now, once more
//   done <id>          the frame was read (or wasn't wanted)
//   state <words>      for ft-screens' log
#pragma once

#include "blit.h"
#include "feed.h"

namespace media {

class ScreenFeed : public Feed {
public:
    // `fd`: this end of the socket; it stays the caller's.
    explicit ScreenFeed(int fd) : fd_(fd) {}

    int fd() const override { return fd_; }
    bool Serve() override;
    bool Begin(const Pictures &pictures, std::string &err) override;
    void End() override;
    void Fresh() override;
    double Wait(double) const override { return pending_ ? 0 : 1; }
    bool Draw(Pictures &pictures, int into, double now) override;
    void Skip(double now) override;

    // Says something to ft-screens ("state ...").
    void Say(const std::string &words) const;

private:
    void Message(const char *msg, const int *fds, int nfds);
    void Done();

    int fd_;
    Blit blit_;
    bool wanted_ = false;
    bool pending_ = false;   // a frame ft-screens waits to hear "done" of
    uint32_t frame_ = 0;
    int refused_ = 0;        // buffers the GPU wouldn't import, for the log
};

}  // namespace media
