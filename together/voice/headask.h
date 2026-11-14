// Where the listener's head is on their own floor, asked of ft-screens (its `head`: "ok X Y Z
// YAW", metres and degrees, the way SteamVR's standing universe has it) ten times a second.
// ft-screens tracks the headset anyway; this program needs no SteamVR of its own for it.
//
// Nothing here waits: Tick asks what is due and takes what came.
#pragma once

#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace voice {

class HeadAsk {
public:
    static constexpr double kEveryS = 0.1;
    static constexpr double kFreshS = 1.0;   // an answer older than this isn't where the head is

    // `screens`: ft-screens' socket's name. False when that isn't one.
    bool Open(const std::string &screens);
    void Close();
    void Tick(double now);
    int fd() const { return sock_; }

    bool known(double now) const { return now - heard_ <= kFreshS; }
    const double *at() const { return at_; }   // x, y, z
    double yaw() const { return yaw_; }        // radians about the way up, 0 faces -z
    HeadAsk() = default;
    HeadAsk(const HeadAsk &) = delete;
    HeadAsk &operator=(const HeadAsk &) = delete;
    ~HeadAsk() { Close(); }

private:
    int sock_ = -1;
    sockaddr_un screens_{};
    socklen_t screensLen_ = 0;
    double asked_ = -1e9, heard_ = -1e9;
    double at_[3] = {}, yaw_ = 0;
};

// What `head` answered, read: false when it isn't a head's place.
bool ReadHead(const std::string &reply, double at[3], double &yaw);

}  // namespace voice
