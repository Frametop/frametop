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
    }
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
        if (!in.ok || p.id >= kPeople || taken[p.id] || (p.away & ~(kAwayGame | kAwayOff))) return false;
        taken[p.id] = true;
    }
    return m.owner < kPeople && taken[m.owner];
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
