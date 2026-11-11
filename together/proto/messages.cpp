// The control stream's messages, to bytes and back (messages.h).
#include "messages.h"

namespace proto {
namespace {

constexpr size_t kFrame = 6;   // u32 length, u16 kind

std::vector<uint8_t> Framed(uint16_t kind, const wire::Out &body) {
    wire::Out out;
    out.U32(uint32_t(body.b.size()));
    out.U16(kind);
    out.Bytes(body.b.data(), body.b.size());
    return out.b;
}

void Put(wire::Out &out, const Look &m) {
    out.Bytes(m.hash, kLookHash);
    out.U32(m.size);
}

// No look is all zeros: there is one way to say it.
bool Take(wire::In &in, Look &m) {
    in.Take(m.hash, kLookHash);
    m.size = in.U32();
    if (!in.ok || m.size > kMostLook) return false;
    if (m.size) return true;
    for (uint8_t b : m.hash)
        if (b) return false;
    return true;
}

void Put(wire::Out &out, const Room &m) {
    out.Bytes(m.room, kRoomId);
    out.U32(m.turn);
    out.U8(m.owner);
    out.U8(uint8_t(m.people.size()));
    for (const Person &p : m.people) {
        out.U8(p.id);
        out.Text(p.name, kMostName), out.Text(p.login, kMostLogin), out.Text(p.address, kMostAddress);
        out.U16(p.port);
        out.U8(p.away);
        for (float v : p.spot.at) out.F32(v);
        out.F32(p.spot.yaw);
        Put(out, p.look);
    }
    out.U8(uint8_t(m.panels.size()));
    for (const Panel &p : m.panels) panelwire::Put(out, p);
}

bool Take(wire::In &in, Room &m) {
    in.Take(m.room, kRoomId);
    m.turn = in.U32();
    m.owner = in.U8();
    const size_t count = in.U8();
    if (!in.ok || count < 1 || count > size_t(kPeople)) return false;
    m.people.assign(count, Person{});
    bool taken[kPeople] = {};
    for (Person &p : m.people) {
        p.id = in.U8();
        in.Text(p.name, kMostName), in.Text(p.login, kMostLogin), in.Text(p.address, kMostAddress);
        p.port = in.U16();
        p.away = in.U8();
        for (float &v : p.spot.at) v = in.F32(-kFar, kFar);
        p.spot.yaw = in.F32(-7, 7);
        if (!Take(in, p.look)) return false;
        if (!in.ok || p.id >= kPeople || taken[p.id] || (p.away & ~(kAwayGame | kAwayOff))) return false;
        taken[p.id] = true;
    }
    if (m.owner >= kPeople || !taken[m.owner]) return false;
    const size_t panels = in.U8();
    if (!in.ok || panels > size_t(kMostPanels)) return false;
    m.panels.assign(panels, Panel{});
    int last = -1;
    for (Panel &p : m.panels) {
        if (!panelwire::Take(in, p, kPeople) || !taken[p.owner()] || p.id <= last) return false;
        last = p.id;
    }
    return true;
}

}  // namespace

std::vector<uint8_t> Pack(const Hello &m) {
    wire::Out out;
    out.U16(m.version);
    out.Bytes(m.room, kRoomId);
    out.Text(m.code, kMostCode), out.Text(m.name, kMostName);
    out.U16(m.port);
    return Framed(kHello, out);
}

std::vector<uint8_t> Pack(const Welcome &m) {
    wire::Out out;
    out.U8(m.you);
    Put(out, m.room);
    return Framed(kWelcome, out);
}

std::vector<uint8_t> Pack(const Refuse &m) {
    wire::Out out;
    out.U8(uint8_t(m.why));
    out.Text(m.text, kMostWhy);
    return Framed(kRefuse, out);
}

std::vector<uint8_t> Pack(const Room &m) {
    wire::Out out;
    Put(out, m);
    return Framed(kRoom, out);
}

std::vector<uint8_t> Pack(const Away &m) {
    wire::Out out;
    out.U8(m.away);
    return Framed(kAway, out);
}

std::vector<uint8_t> Pack(const Panel &m) {
    wire::Out out;
    panelwire::Put(out, m);
    return Framed(kPanel, out);
}

std::vector<uint8_t> Pack(const Unpanel &m) {
    wire::Out out;
    out.U8(m.panel);
    return Framed(kUnpanel, out);
}

std::vector<uint8_t> Pack(const Watch &m) {
    wire::Out out;
    out.U8(m.panel), out.U8(m.level), out.U8(m.fresh);
    return Framed(kWatch, out);
}

std::vector<uint8_t> Pack(const Look &m) {
    wire::Out out;
    Put(out, m);
    return Framed(kLook, out);
}

std::vector<uint8_t> Pack(const Want &m) {
    wire::Out out;
    out.Bytes(m.hash, kLookHash);
    return Framed(kWant, out);
}

std::vector<uint8_t> PackLeave() { return Framed(kLeave, wire::Out{}); }

bool Read(const std::vector<uint8_t> &body, Hello &m) {
    wire::In in(body.data(), body.size());
    m = Hello{};
    m.version = in.U16();
    if (in.ok && m.version != kVersion) return true;
    in.Take(m.room, kRoomId);
    in.Text(m.code, kMostCode), in.Text(m.name, kMostName);
    m.port = in.U16();
    return in.done();
}

bool Read(const std::vector<uint8_t> &body, Welcome &m) {
    wire::In in(body.data(), body.size());
    m = Welcome{};
    m.you = in.U8();
    return Take(in, m.room) && in.done() && m.room.person(m.you) && m.you != m.room.owner;
}

bool Read(const std::vector<uint8_t> &body, Refuse &m) {
    wire::In in(body.data(), body.size());
    m = Refuse{};
    const uint8_t why = in.U8();
    m.why = why >= uint8_t(Why::kVersion) && why <= uint8_t(Why::kBad) ? Why(why) : Why::kBad;
    return in.Text(m.text, kMostWhy) && in.ok;   // a later version may say more after it
}

bool Read(const std::vector<uint8_t> &body, Room &m) {
    wire::In in(body.data(), body.size());
    m = Room{};
    return Take(in, m) && in.done();
}

bool Read(const std::vector<uint8_t> &body, Away &m) {
    wire::In in(body.data(), body.size());
    m.away = in.U8();
    return in.done() && !(m.away & ~(kAwayGame | kAwayOff));
}

bool Read(const std::vector<uint8_t> &body, Panel &m) {
    wire::In in(body.data(), body.size());
    m = Panel{};
    return panelwire::Take(in, m, kPeople) && in.done();
}

bool Read(const std::vector<uint8_t> &body, Unpanel &m) {
    wire::In in(body.data(), body.size());
    m.panel = in.U8();
    return in.done() && m.panel < kPeople * kPanelNumbers;
}

bool Read(const std::vector<uint8_t> &body, Watch &m) {
    wire::In in(body.data(), body.size());
    m.panel = in.U8(), m.level = in.U8();
    const uint8_t fresh = in.U8();
    m.fresh = fresh == 1;
    return in.done() && m.panel < kPeople * kPanelNumbers && m.level <= kFocused && fresh <= 1;
}

bool Read(const std::vector<uint8_t> &body, Look &m) {
    wire::In in(body.data(), body.size());
    m = Look{};
    return Take(in, m) && in.done();
}

bool Read(const std::vector<uint8_t> &body, Want &m) {
    wire::In in(body.data(), body.size());
    in.Take(m.hash, kLookHash);
    return in.done();
}

bool Parser::Feed(const uint8_t *data, size_t n) {
    if (broken_) return false;
    if (at_ > 0 && at_ == held_.size()) held_.clear(), at_ = 0;
    held_.insert(held_.end(), data, data + n);
    // A message that could never be whole breaks the stream as soon as its length is seen,
    // before its body is waited for.
    size_t at = at_;
    while (held_.size() - at >= kFrame) {
        wire::In in(held_.data() + at, kFrame);
        const size_t body = in.U32();
        if (body > kMostMessage) return broken_ = true, held_.clear(), at_ = 0, false;
        if (held_.size() - at - kFrame < body) break;
        at += kFrame + body;
    }
    if (at_ > kMostMessage) held_.erase(held_.begin(), held_.begin() + long(at_)), at_ = 0;
    return true;
}

bool Parser::Next(uint16_t &kind, std::vector<uint8_t> &body) {
    if (broken_ || held_.size() - at_ < kFrame) return false;
    wire::In in(held_.data() + at_, kFrame);
    const size_t n = in.U32();
    kind = in.U16();
    if (held_.size() - at_ - kFrame < n) return false;
    const uint8_t *from = held_.data() + at_ + kFrame;
    body.assign(from, from + n);
    at_ += kFrame + n;
    return true;
}

}  // namespace proto
