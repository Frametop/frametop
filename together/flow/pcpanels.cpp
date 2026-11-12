// The panels of the wearer's split compute PC (pcpanels.h).
#include "pcpanels.h"

namespace flow {

bool PcPanels::Refuse(int link, proto::Why why, const char *text) {
    proto::Refuse r;
    r.why = why, r.text = text;
    links_.Send(link, proto::Pack(r));
    return false;
}

bool PcPanels::Said(int link, const std::string &address, uint16_t kind, const std::vector<uint8_t> &body, Video &video, double now) {
    if (link == link_) return link_ = -1, false;   // once it is in, a PC has nothing more to say here
    proto::Sender sender;
    session::Who who;
    if (kind != proto::kSender || !proto::Read(body, sender)) return false;
    if (sender.version != proto::kVersion) return Refuse(link, proto::Why::kVersion, "This PC and the headset run different versions of Frametop Together. Update Frametop on both.");
    if (!tailnet_.WhoIs(address, who)) return Refuse(link, proto::Why::kBad, "This PC isn't on the headset's tailnet.");
    if (invites_.Use(sender.code, address, now) != room::Invites::Answer::kGood)
        return Refuse(link, proto::Why::kCode, "That code isn't good. A code works once, for ten minutes.");
    if (link_ >= 0) session_.Hang(link_);
    link_ = link;
    for (int number = room::kFramePanels; number < room::kOwnPanels; ++number) {
        panels_[number - room::kFramePanels] = Panel{};
        video.SourceCame(number);   // the PC hears what each is to be encoded at
    }
    return true;
}

bool PcPanels::Frame(int link, const uint8_t *data, size_t n, Video &video, double now) {
    if (link < 0 || link != link_) return false;
    proto::FrameHead head;
    const uint8_t *unit = nullptr;
    size_t size = 0;
    // (where a Frame says a panel's id, a PC says its number)
    if (!proto::ReadFrame(data, n, head, unit, size) || !Pcs(head.panel) || !panels_[head.panel - room::kFramePanels].allowance.Spend(now)) return ++dropped_, true;
    Order::Frame frame;
    frame.number = head.number, frame.key = head.key, frame.unit.assign(unit, unit + size);
    std::vector<Order::Frame> pass;
    panels_[head.panel - room::kFramePanels].order.Take(std::move(frame), now, pass);
    for (const Order::Frame &f : pass) video.Unit(head.panel, f.key, f.unit.data(), f.unit.size(), now);
    return true;
}

void PcPanels::Down(int link) {
    if (link == link_) link_ = -1;
}

void PcPanels::Tick(double now) {
    for (int number = room::kFramePanels; number < room::kOwnPanels; ++number)
        if (panels_[number - room::kFramePanels].order.WantsKey(now)) Say(number, proto::kEncodeKey, 0);
}

void PcPanels::Say(int number, uint8_t word, uint32_t value) {
    if (link_ < 0) return;
    proto::Encode e;
    e.number = uint8_t(number), e.word = word, e.value = value;
    links_.Send(link_, proto::Pack(e));
}

void PcPanels::Rate(int number, int bitrate) { Pcs(number) ? Say(number, proto::kEncodeRate, uint32_t(bitrate)) : frames_.Rate(number, bitrate); }
void PcPanels::Full(int number, int aSecond) { Pcs(number) ? Say(number, proto::kEncodeFull, uint32_t(aSecond)) : frames_.Full(number, aSecond); }
void PcPanels::Pictures(int number, int aSecond) { Pcs(number) ? Say(number, proto::kEncodePictures, uint32_t(aSecond)) : frames_.Pictures(number, aSecond); }
void PcPanels::Key(int number) { Pcs(number) ? Say(number, proto::kEncodeKey, 0) : frames_.Key(number); }
void PcPanels::Watched(int number, bool watched) { Pcs(number) ? Say(number, proto::kEncodeWatched, watched ? 1 : 0) : frames_.Watched(number, watched); }

}  // namespace flow
