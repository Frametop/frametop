// The messages on a Frametop Together link's control stream (docs/together.md, "The protocol"):
// who is calling, whether they may come in, and the room's people. Each is framed as
//   u32 the body's length, u16 its kind, the body
// little-endian (bytes.h). A reader takes nothing a peer says on trust: every count, size and
// number is bounded here, before the session sees it.
//
// No sockets, no msquic: the bytes only.
#pragma once

#include "bytes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace proto {

constexpr uint16_t kVersion = 1;   // peers on another version are refused (Why::kVersion)
constexpr int kPeople = 4;         // in a room, its owner among them
constexpr size_t kRoomId = 16;
constexpr size_t kMostMessage = 4096;   // a body's bytes: a longer one breaks the stream
constexpr size_t kMostName = 48, kMostLogin = 96, kMostAddress = 45, kMostCode = 16, kMostWhy = 120;
constexpr float kFar = 100;        // metres from the room's middle: nothing is further

enum Kind : uint16_t { kHello = 1, kWelcome = 2, kRefuse = 3, kRoom = 4, kAway = 5, kLeave = 6 };

// Why someone isn't let in. The numbers and Refuse's layout stay as they are in every version,
// so that a peer on another one can still be told to update.
enum class Why : uint8_t { kVersion = 1, kFull = 2, kCode = 3, kDeclined = 4, kNotOpen = 5, kNotInRoom = 6, kBad = 7 };

constexpr uint8_t kAwayGame = 1, kAwayOff = 2;   // in a VR game; the headset is off their head

// Where someone stands in the room, and the way they face: radians about y, 0 faces -z.
struct Spot {
    float at[3] = {0, 0, 0};
    float yaw = 0;
};

// The first message each way. A caller with an invite sends its code; one who is in the room
// already (two guests linking up) names the room.
struct Hello {
    uint16_t version = kVersion;
    uint8_t room[kRoomId] = {};
    std::string code, name;
    uint16_t port = 0;   // where the sender listens, for the others in the room to call
};

struct Person {
    uint8_t id = 0;   // their place among the room's kPeople, which is also their colour
    std::string name, login, address;
    uint16_t port = 0;
    uint8_t away = 0;
    Spot spot;
};

// The room as its owner has it. `turn` counts its changes: a guest keeps the newest.
struct Room {
    uint8_t room[kRoomId] = {};
    uint32_t turn = 0;
    uint8_t owner = 0;
    std::vector<Person> people;

    const Person *person(int id) const {
        for (const Person &p : people)
            if (p.id == id) return &p;
        return nullptr;
    }
};

struct Welcome {
    uint8_t you = 0;
    Room room;
};

struct Refuse {
    Why why = Why::kBad;
    std::string text;   // for a person to read
};

struct Away {
    uint8_t away = 0;
};

// A message, framed, ready to send. Our own text is cut to its limit (wire::Clean).
std::vector<uint8_t> Pack(const Hello &m);
std::vector<uint8_t> Pack(const Welcome &m);
std::vector<uint8_t> Pack(const Refuse &m);
std::vector<uint8_t> Pack(const Room &m);
std::vector<uint8_t> Pack(const Away &m);
std::vector<uint8_t> PackLeave();

// A message's body read. False when it isn't one: too short or too long, a count or a number
// out of bounds, text that isn't text, two people in one place. A Hello of another version
// reads as far as its version and is true: the rest of it is not ours to understand.
bool Read(const std::vector<uint8_t> &body, Hello &m);
bool Read(const std::vector<uint8_t> &body, Welcome &m);
bool Read(const std::vector<uint8_t> &body, Refuse &m);
bool Read(const std::vector<uint8_t> &body, Room &m);
bool Read(const std::vector<uint8_t> &body, Away &m);

// A control stream's bytes cut back into messages.
class Parser {
public:
    // The stream's next bytes. False when it is broken (a body longer than kMostMessage): the
    // link is to be closed, and nothing more is taken.
    bool Feed(const uint8_t *data, size_t n);
    // The next whole message, in order. False when there is none yet.
    bool Next(uint16_t &kind, std::vector<uint8_t> &body);

private:
    std::vector<uint8_t> held_;
    size_t at_ = 0;
    bool broken_ = false;
};

}  // namespace proto
