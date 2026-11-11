// A program's end of ft-together's local socket (together/README.md, "ft-together"), for one
// that draws the room or tells it where its wearer is: it keeps asking who is in the room,
// watches the others' poses, sees to it that your floor is laid in the room (floor.h), and
// hands your own pose over.
//
// One socket, bound to a name of the program's own, does it all: ft-together answers to it and
// sends the watched poses to it, and whatever comes from anyone else is a question to this
// program (its own `state`, `quit`), kept for its loop.
//
// Nothing here waits: Tick asks what is due and takes what came. An ft-together that isn't
// there, or stops answering, is a room nobody is in.
#pragma once

#include "floor.h"
#include "people.h"

#include <deque>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace roomclient {

// A question another program asked this one.
struct Question {
    std::string text;
    sockaddr_un from;
    socklen_t fromLen = 0;
};

class Client {
public:
    static constexpr double kStateS = 0.5;    // between questions about the room
    static constexpr double kWatchS = 1.0;    // ...and between asks to watch (ft-together forgets a watcher after five)
    static constexpr double kFloorS = 0.5;    // between questions about the floor while none is known,
    static constexpr double kFloorKnownS = 2; // and once one is: ft-together's is the one that counts
    static constexpr double kSilentS = 2.5;   // no word of the room for this long: there is none
    static constexpr size_t kMostQuestions = 16;

    // Binds @`own`, to talk to @`together`. `watch`: the others' poses are wanted. False with
    // `err` when the name is taken (the program runs already) or isn't one.
    bool Open(const std::string &own, const std::string &together, bool watch, std::string &err);
    void Close();
    Client() = default;
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    ~Client() { Close(); }

    // Call every pass of the loop, `now` in seconds. `head`: where the wearer's head is on
    // their own floor now, x, z and the way it faces (roomfloor::Facing), or nullptr while that
    // isn't known. The room's floor is laid from it when the room has none yet.
    void Tick(double now, const double *head);

    const roompeople::People &people() const { return people_; }
    bool laid() const { return laid_; }
    const roomfloor::Floor &floor() const { return floor_; }
    // Your own pose, in the room's directions. False when it didn't go: ft-together isn't there.
    bool Send(const proto::Pose &pose);

    // The next question to this program, if one waits; and its answer.
    bool Next(Question &out);
    void Reply(const Question &to, const std::string &text);

    unsigned posesIn() const { return posesIn_; }   // counted since Open

private:
    void Take(double now);
    void Answer(const std::string &text, double now);
    void Ask(const std::string &text);

    int sock_ = -1;
    sockaddr_un together_{};
    socklen_t togetherLen_ = 0;
    bool watch_ = false, laid_ = false, none_ = false;   // none_: ft-together said the room has no floor yet
    roompeople::People people_;
    roomfloor::Floor floor_;
    double asked_ = -1e9, watched_ = -1e9, floored_ = -1e9, heard_ = -1e9;
    std::deque<Question> questions_;
    unsigned posesIn_ = 0;
};

}  // namespace roomclient
