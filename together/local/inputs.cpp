// Other people's input on its way to this Frame's desktop (inputs.h).
#include "inputs.h"

namespace local {

void Inputs::Input(int person, const proto::Act &act) {
    if (behind_) {
        proto::Act letGo;
        letGo.what = proto::kActLetGo;
        const std::vector<uint8_t> body = proto::Body(letGo);
        if (takers_.Send(sock_, kNobody, body.data(), body.size(), now_)) return void(++missed_);
        behind_ = false;
    }
    const std::vector<uint8_t> body = proto::Body(act);
    if (takers_.Send(sock_, person, body.data(), body.size(), now_)) behind_ = true, ++missed_;
}

}  // namespace local
