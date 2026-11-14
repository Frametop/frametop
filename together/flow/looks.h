// The avatars the room's people have on, through ft-together (docs/together.md, "Avatars"). The
// room says what each one has on: a model file's size and SHA-256 (proto::Look). Whoever doesn't
// hold that file asks its wearer for it, on the link to them, and it comes on a stream of its
// own.
//
// Out: the wearer's file goes only to someone in the room who asks for the one the wearer has
// on now, and only so often to one link. In: a file is taken only from the one the room says
// has it on, only after it was asked for, and it is kept only if its bytes are that size and
// hash. One that doesn't come is asked for again later; someone who changes avatars all the
// time is followed only so often. What a peer sends is never parsed here: the file is bytes
// with a hash until the program that draws it reads it (avatar/modelfile.h).
//
// The rules only. The links and the files are handed in (net/, local/wardrobe.h), so two of
// these can be run against each other in a test.
#pragma once

#include "../room/allowance.h"
#include "../session/session.h"

#include <map>
#include <memory>
#include <vector>

namespace flow {

using FileBytes = std::shared_ptr<const std::vector<uint8_t>>;

// What a file is as a look: its size and SHA-256. None for no bytes, or more than a look may be.
proto::Look LookOf(const uint8_t *data, size_t n);

// The links, as the avatars' files use them.
class FileWires {
public:
    virtual ~FileWires() = default;
    // From now on one file of `bytes` at most is taken from the link's far end.
    virtual void Await(int link, size_t bytes) = 0;
    // A file to the link's far end. False when it can't be sent now.
    virtual bool File(int link, const FileBytes &file) = 0;
};

// The avatars' files on this Frame.
class Closet {
public:
    virtual ~Closet() = default;
    // The file the wearer has on, if it is `look`.
    virtual FileBytes Worn(const proto::Look &look) = 0;
    // Whether the file of a look someone else has on is here.
    virtual bool Has(const proto::Look &look) const = 0;
    // Keeps one that came. Its bytes are `look`'s.
    virtual void Keep(const proto::Look &look, const std::vector<uint8_t> &bytes) = 0;
    // Only these are still worn by the others in the room: the rest of the kept files go.
    virtual void Only(const std::vector<proto::Look> &worn) = 0;
};

class Looks {
public:
    // A file that didn't come is asked for again after this long, and as long again as it
    // would take on the slowest link worth having.
    static constexpr double kAskAgainS = 30, kSlowestBytesASecond = 64 << 10;
    // How often one person's files are asked for, and ours is sent to one link.
    static constexpr double kAsksASecond = 1.0 / 30, kAsksAtOnce = 3;
    static constexpr double kSendsASecond = 1.0 / 30, kSendsAtOnce = 3;

    Looks(session::Session &session, FileWires &wires, Closet &closet) : session_(session), wires_(wires), closet_(closet) {}

    // From the links. Wanted is the session's word that a member asks for a file.
    void Wanted(int link, const proto::Want &want, double now);
    void File(int link, const std::vector<uint8_t> &bytes, double now);
    void Down(int link) { sent_.erase(link); }
    // Now and then (a second apart is enough).
    void Tick(double now);

    size_t refused() const { return refused_; }   // files that came and weren't kept

private:
    struct Asked {
        proto::Look look;
        int link = -1;
        double at = -1e9;
        room::Allowance allowance{kAsksASecond, kAsksAtOnce};
    };

    session::Session &session_;
    FileWires &wires_;
    Closet &closet_;
    Asked asked_[proto::kPeople];
    std::map<int, room::Allowance> sent_;
    size_t refused_ = 0;
};

}  // namespace flow
