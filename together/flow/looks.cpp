// The avatars the room's people have on, through ft-together (looks.h).
#include "looks.h"

#include <cstring>
#include <openssl/evp.h>

namespace flow {

proto::Look LookOf(const uint8_t *data, size_t n) {
    proto::Look look;
    unsigned int size = 0;
    if (!data || n == 0 || n > proto::kMostLook) return look;
    static_assert(proto::kLookHash == 32, "a look's hash is SHA-256");
    if (EVP_Digest(data, n, look.hash, &size, EVP_sha256(), nullptr) != 1 || size != proto::kLookHash) return proto::Look{};
    look.size = uint32_t(n);
    return look;
}

void Looks::Wanted(int link, const proto::Want &want, double now) {
    const proto::Look &mine = session_.look();
    if (session_.PersonOn(link) < 0 || mine.none() || std::memcmp(want.hash, mine.hash, proto::kLookHash)) return;
    if (!sent_.try_emplace(link, kSendsASecond, kSendsAtOnce).first->second.Spend(now)) return;
    if (const FileBytes file = closet_.Worn(mine)) wires_.File(link, file);
}

void Looks::File(int link, const std::vector<uint8_t> &bytes, double) {
    const int person = session_.PersonOn(link);
    const proto::Person *p = person >= 0 ? session_.room().person(person) : nullptr;
    // a file is the one that was asked of its sender, which they still have on, or it is nothing
    if (!p || p->look.none() || asked_[person].link != link || !proto::SameLook(asked_[person].look, p->look) || bytes.size() != p->look.size ||
        !proto::SameLook(LookOf(bytes.data(), bytes.size()), p->look)) {
        ++refused_;
        return;
    }
    closet_.Keep(p->look, bytes);
}

void Looks::Tick(double now) {
    std::vector<proto::Look> worn;
    const int me = session_.me();
    for (int id = 0; id < proto::kPeople; ++id) {
        const proto::Person *p = me >= 0 ? session_.room().person(id) : nullptr;
        if (!p) asked_[id] = Asked{};   // the place's next person starts anew
        if (!p || id == me || p->look.none()) continue;
        worn.push_back(p->look);
        const int link = session_.LinkTo(id);
        if (link < 0 || closet_.Has(p->look)) continue;
        Asked &a = asked_[id];
        const bool again = a.link == link && proto::SameLook(a.look, p->look);
        if (again && now - a.at < kAskAgainS + p->look.size / kSlowestBytesASecond) continue;
        if (!a.allowance.Spend(now)) continue;
        a.look = p->look, a.link = link, a.at = now;
        wires_.Await(link, p->look.size);
        session_.Want(id, p->look);
    }
    closet_.Only(worn);
    if (me < 0) sent_.clear();
}

}  // namespace flow
