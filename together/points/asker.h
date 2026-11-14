// ft-roompoints' socket: one datagram socket, bound to a name of its own, that asks ft-together
// for the others' pointers (`points`: it sends them for five seconds after each ask) and
// ft-roompanels which panel here shows each of the room's (`panels`), and takes what comes:
// with the pointers, where the others look.
// What comes from anyone else is a question to this program, kept for its loop.
//
// Nothing here waits. An ft-roompanels that stops answering is a room with no panels here.
#pragma once

#include "marks.h"
#include "panelkeys.h"

#include <deque>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace points {

struct Question {
    std::string text;
    sockaddr_un from;
    socklen_t fromLen = 0;
};

class Asker {
public:
    static constexpr double kPointsS = 1.0;   // between asks for the pointers
    static constexpr double kPanelsS = 0.5;   // ...and for the panels
    static constexpr double kSilentS = 3.0;   // no good word of the panels for this long: none are known
    static constexpr size_t kMostQuestions = 16;

    // Binds @`own`. False with `err` when the name is taken (the program runs already) or one
    // of the three isn't a socket's name.
    bool Open(const std::string &own, const std::string &together, const std::string &panels, std::string &err);
    void Close();
    int fd() const { return sock_; }
    Asker() = default;
    Asker(const Asker &) = delete;
    Asker &operator=(const Asker &) = delete;
    ~Asker() { Close(); }

    // Every pass of the loop: asks what is due, and gives what came to `marks` (where the
    // others point), `glances` (where they look) and `keys`.
    void Tick(double now, Marks &marks, Marks &glances, PanelKeys &keys);

    bool Next(Question &out);
    void Reply(const Question &to, const std::string &text);

    unsigned pointsIn() const { return pointsIn_; }   // pointer datagrams taken since Open

private:
    void Ask(const sockaddr_un &to, socklen_t len, const char *text);

    int sock_ = -1;
    sockaddr_un together_{}, panels_{};
    socklen_t togetherLen_ = 0, panelsLen_ = 0;
    double pointsAsked_ = -1e9, panelsAsked_ = -1e9, panelsHeard_ = -1e9;
    std::deque<Question> questions_;
    unsigned pointsIn_ = 0;
};

}  // namespace points
