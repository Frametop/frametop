// The messages on a Frametop Together link's control stream (docs/together.md, "The protocol"):
// who is calling, whether they may come in, the room's people and the panels they share. Each is
// framed as
//   u32 the body's length, u16 its kind, the body
// little-endian (bytes.h). A reader takes nothing a peer says on trust: every count, size and
// number is bounded here, before the session sees it.
//
// No sockets, no msquic: the bytes only.
#pragma once

#include "bytes.h"
#include "panel.h"

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
static_assert(kPanelFar == kFar, "a panel is no further than a person");

// 12 to 14 are control's (control.h) and 15 is a panel moved (move.h): the session passes them
// on unread to whoever knows them.
enum Kind : uint16_t {
    kHello = 1, kWelcome = 2, kRefuse = 3, kRoom = 4, kAway = 5, kLeave = 6, kPanel = 7, kUnpanel = 8, kWatch = 9, kLook = 10, kWant = 11,
    kAskControl = 12, kGrant = 13, kAct = 14, kMove = 15,
};

// Why someone isn't let in. The numbers and Refuse's layout stay as they are in every version,
// so that a peer on another one can still be told to update.
enum class Why : uint8_t { kVersion = 1, kFull = 2, kCode = 3, kDeclined = 4, kNotOpen = 5, kNotInRoom = 6, kBad = 7 };

constexpr uint8_t kAwayGame = 1, kAwayOff = 2;   // in a VR game; the headset is off their head

// Where someone stands in the room, and the way they face: radians about y, 0 faces -z.
struct Spot {
    float at[3] = {0, 0, 0};
    float yaw = 0;
};

// The avatar someone has on, as the others may ask them for it: its model file's size and
// SHA-256. No size is no avatar to send, and they are drawn as the abstract figure. The file
// itself goes on a one-way stream of its own, whose first byte is kLookStream (framewire.h has a
// picture's), to whoever asked its wearer with a Want.
constexpr size_t kLookHash = 32;
constexpr uint32_t kMostLook = 24u << 20;   // a model file's bytes: a larger one isn't sent, asked for or taken
constexpr uint8_t kLookStream = 3;
struct Look {
    uint8_t hash[kLookHash] = {};
    uint32_t size = 0;
    bool none() const { return size == 0; }
};
inline bool SameLook(const Look &a, const Look &b) { return a.size == b.size && !std::memcmp(a.hash, b.hash, kLookHash); }

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
    Look look;
};

// The room as its owner has it. `turn` counts its changes: a guest keeps the newest. The panels
// are in the order of their ids, each one's owner among the people.
struct Room {
    uint8_t room[kRoomId] = {};
    uint32_t turn = 0;
    uint8_t owner = 0;
    std::vector<Person> people;
    std::vector<Panel> panels;

    const Person *person(int id) const {
        for (const Person &p : people)
            if (p.id == id) return &p;
        return nullptr;
    }
    const Panel *panel(int id) const {
        for (const Panel &p : panels)
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

// A guest's panel, to the room's owner: shared, moved or changed (a Panel, panel.h), or taken
// back (an Unpanel). The owner's next Room has it, or doesn't.
struct Unpanel {
    uint8_t panel = 0;
};

// A look's file is asked of the one who has it on, on the link to them: the Look is a guest's
// word to the room's owner on what they have on (the owner's next Room has it).
struct Want {
    uint8_t hash[kLookHash] = {};
};

// What a viewer has of a panel, to the Frame that sends it: whether they look at it, and
// whether their decoder needs a keyframe to go on.
constexpr uint8_t kHidden = 0, kInView = 1, kFocused = 2;
struct Watch {
    uint8_t panel = 0;
    uint8_t level = kHidden;
    bool fresh = false;
};

// A body framed as a message of `kind`.
inline std::vector<uint8_t> Framed(uint16_t kind, const wire::Out &body) {
    wire::Out out;
    out.U32(uint32_t(body.b.size()));
    out.U16(kind);
    out.Bytes(body.b.data(), body.b.size());
    return out.b;
}

// A message, framed, ready to send. Our own text is cut to its limit (wire::Clean).
std::vector<uint8_t> Pack(const Hello &m);
std::vector<uint8_t> Pack(const Welcome &m);
std::vector<uint8_t> Pack(const Refuse &m);
std::vector<uint8_t> Pack(const Room &m);
std::vector<uint8_t> Pack(const Away &m);
std::vector<uint8_t> Pack(const Panel &m);
std::vector<uint8_t> Pack(const Unpanel &m);
std::vector<uint8_t> Pack(const Watch &m);
std::vector<uint8_t> Pack(const Look &m);
std::vector<uint8_t> Pack(const Want &m);
std::vector<uint8_t> PackLeave();

// A message's body read. False when it isn't one: too short or too long, a count or a number
// out of bounds, text that isn't text, two people in one place, a panel of someone who isn't
// in the room, a look larger than one may be or a hash with no size. A Hello of another version reads as far as its version and is true: the rest of
// it is not ours to understand.
bool Read(const std::vector<uint8_t> &body, Hello &m);
bool Read(const std::vector<uint8_t> &body, Welcome &m);
bool Read(const std::vector<uint8_t> &body, Refuse &m);
bool Read(const std::vector<uint8_t> &body, Room &m);
bool Read(const std::vector<uint8_t> &body, Away &m);
bool Read(const std::vector<uint8_t> &body, Panel &m);
bool Read(const std::vector<uint8_t> &body, Unpanel &m);
bool Read(const std::vector<uint8_t> &body, Watch &m);
bool Read(const std::vector<uint8_t> &body, Look &m);
bool Read(const std::vector<uint8_t> &body, Want &m);

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
