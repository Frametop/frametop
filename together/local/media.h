// This Frame's media programs, as ft-together has them (mediawire.h says what is said): the
// ones that encode the wearer's shared panels and the ones that show other people's, each on a
// connection of its own to the stream socket "@ft_together_media". What they hand over goes to
// the video's rules (flow/video.h), and what the rules have for them goes back.
//
// Only this user's programs are let in, and one for each panel: a second one that says the
// same panel takes the first one's place. A program that says what it may not, says nothing
// about itself for kHelloS seconds, or doesn't take what is sent to it (kMostOut bytes waiting)
// is let go: the socket is closed, and it may come again.
//
// Nothing here waits: the sockets are non-blocking, and Serve is run from ft-together's loop.
#pragma once

#include "../flow/video.h"
#include "mediawire.h"

#include <poll.h>
#include <string>
#include <vector>

namespace local {

class Media : public flow::Programs {
public:
    static constexpr size_t kMostClients = 24;
    static constexpr size_t kMostOut = 4u << 20;    // bytes waiting for a program to take them
    static constexpr size_t kMostAPass = 4u << 20;  // bytes taken from one program before the next has its turn
    static constexpr double kHelloS = 5;

    Media() = default;
    ~Media() override;
    Media(const Media &) = delete;
    Media &operator=(const Media &) = delete;

    // Listens at "@name". False, with errno set, when it can't.
    bool Listen(const std::string &name);
    // What the loop is to wait on, added to `fds`.
    void Fds(std::vector<pollfd> &fds) const;
    // New programs let in, what the programs sent handed to `video`, and what waits for them
    // sent. With no video (no session yet) nobody is let in.
    void Serve(flow::Video *video, double now);
    size_t clients() const { return clients_.size(); }
    // Which panels have a program: {"sources":[numbers],"sinks":[panel ids]}.
    std::string State() const;

    void Rate(int number, int bitrate) override;
    void Pictures(int number, int aSecond) override;
    void Key(int number) override;
    void Watched(int number, bool watched) override;
    void Show(uint8_t panel, bool key, const std::vector<uint8_t> &unit) override;
    void Decode(uint8_t panel, bool decode) override;

private:
    enum class Role { kNone, kSource, kSink };
    struct Client {
        int fd = -1;
        Role role = Role::kNone;
        uint8_t which = 0;          // a source's panel number, a sink's panel id
        MediaReader in;
        std::vector<uint8_t> out;   // what waits to be sent, from `sent` on
        size_t sent = 0;
        double since = 0;
        bool gone = false;          // closed at the end of this Serve
    };

    void Accept(bool serving, double now);
    void Handle(Client &c, uint8_t kind, const std::vector<uint8_t> &body, flow::Video &video, double now);
    Client *Find(Role role, int which);
    bool Room(Client &c, size_t more);
    void Flush(Client &c);

    int listen_ = -1;
    std::vector<Client> clients_;
};

}  // namespace local
