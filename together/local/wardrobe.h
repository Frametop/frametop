// The avatars' files on this Frame, as ft-together has them (flow/looks.h): the one the wearer
// has on, and the ones that came from the others in the room.
//
// The wearer's is a model in their avatars folder (avatar/modelfile.h), named by the program
// that draws them (`wear NAME`). The room hears of it, and the others may have its file, only
// if it is a model whose owner lets peers have it (its "may be sent" mark) and no larger than
// a look may be. Any other is worn in private: the others see the abstract figure.
//
// A file that came is kept in a folder of this program's own in $XDG_RUNTIME_DIR, which is
// memory, this user's only, and gone at logout: a sent avatar is held for the session, never
// stored (docs/together.md). It is named by its hash, and goes when nobody in the room has it
// on any more, when the room is left, and when the program ends. Without $XDG_RUNTIME_DIR
// nothing is kept, and everyone is the abstract figure.
#pragma once

#include "../flow/looks.h"

#include <string>
#include <vector>

namespace local {

class Wardrobe : public flow::Closet {
public:
    static constexpr size_t kMostKept = proto::kPeople;   // files at once

    // `name` tells this program's folder from another's (a test runs several).
    explicit Wardrobe(const std::string &name);
    ~Wardrobe() override;
    Wardrobe(const Wardrobe &) = delete;
    Wardrobe &operator=(const Wardrobe &) = delete;

    // The wearer has the model `name` on, or none (""). `look` is what the room is to hear:
    // none unless the file may be sent, and then the answer says why not. Asked again with the
    // same name, the file is read again only if it changed.
    std::string Wear(const std::string &name, proto::Look &look);
    // For the programs that draw and for Settings:
    //   {"wear":"NAME","sent":true|false,"why":"...","people":[{"id":N,"file":"PATH"},...]}
    // with a row for each of the others who has an avatar on: its file here, or "" until it came.
    std::string State(const proto::Room &room, int me) const;

    flow::FileBytes Worn(const proto::Look &look) override;
    bool Has(const proto::Look &look) const override;
    void Keep(const proto::Look &look, const std::vector<uint8_t> &bytes) override;
    void Only(const std::vector<proto::Look> &worn) override;

private:
    std::string Path(const proto::Look &look) const;

    std::string dir_;   // none: nothing is kept
    std::string name_, why_;
    uint64_t stamp_[4] = {};   // the worn file as it was read: device, inode, size, when it changed
    proto::Look wornLook_;
    flow::FileBytes worn_;
    std::vector<proto::Look> kept_;
};

}  // namespace local
