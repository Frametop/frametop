// Who is in the room with you, as ft-together tells it (together/README.md, "ft-together"): the
// room's people from its `state`, each one's newest pose from the datagrams a watcher gets, and
// where the file of the avatar each has on is from its `looks`.
// What ft-together passes on it has checked already (together/local/posegate.h); a pose is read
// again here all the same, since this is another program.
//
// Someone is drawn while they are in the room, aren't you, and a pose of theirs came within the
// last second: a headset that is off, a program that ended and a link that dropped all look the
// same from here, and all end with nobody standing frozen in the room.
//
// The rules only: no sockets, GPU or SteamVR calls here.
#pragma once

#include "../../together/proto/posewire.h"

#include <string>

namespace roompeople {

struct Person {
    bool here = false;        // someone has this place in the room
    std::string name, login;
    int away = 0;             // proto::kAwayGame, kAwayOff
    double spot[3] = {};      // where the room put them: x, z and the way they face (proto::Spot)
    unsigned since = 0;       // counts each newcomer to this place: what was known of the one before isn't theirs
    bool posed = false;
    proto::Pose pose;         // their newest, in the room's directions
    double posedAt = 0;
    std::string avatar;       // the file of the avatar they have on, as ft-together kept it; "" for none, or one that didn't come yet
};

class People {
public:
    static constexpr double kFreshS = 1.0;   // a pose older than this isn't drawn

    // What `state` answered, after its "ok ". False, and nobody is in any room, when it isn't
    // that: ft-together isn't one this program knows.
    bool Room(const std::string &json);
    // What `looks` answered, after its "ok ": where each one's avatar is. False when it isn't
    // that or there is no room, and then nobody has one.
    bool Looks(const std::string &json);
    static constexpr size_t kMostPath = 512;   // a file's name longer than this isn't one
    // ft-together didn't answer: there is no room.
    void Gone();

    // A datagram a watcher got (0x81, whose, the pose) at `now`, seconds. False when it isn't
    // one, or is of nobody in the room.
    bool Pose(const uint8_t *data, size_t n, double now);

    bool inRoom() const { return inRoom_; }
    int me() const { return me_; }   // your own place; -1 outside a room
    const Person &person(int place) const { return people_[place >= 0 && place < proto::kPeople ? place : 0]; }
    // How you are away yourself (proto::kAwayGame, kAwayOff; 0: you are there), as this Frame's
    // own ft-together has it: the room's copy of you trails it by a trip to its owner and back.
    int MyAway() const { return inRoom_ ? myAway_ : 0; }
    // Whether you are away at all: the room is neither drawn for you nor told where you are then.
    bool Away() const { return MyAway() != 0; }
    // Whether the person at `place` is drawn at `now`.
    bool Drawn(int place, double now) const {
        const Person &p = person(place);
        return inRoom_ && place >= 0 && place < proto::kPeople && place != me_ && p.here && p.posed && now - p.posedAt <= kFreshS;
    }

private:
    bool inRoom_ = false;
    int me_ = -1, myAway_ = 0;
    Person people_[proto::kPeople];
};

}  // namespace roompeople
