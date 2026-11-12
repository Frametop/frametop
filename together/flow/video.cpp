// The shared panels' video through ft-together (video.h).
#include "video.h"

#include "../proto/framewire.h"

namespace flow {

// Whether the wearer's panel `number` is in the room now.
bool Video::Mine(int number) const {
    return number >= 0 && number < room::kFramePanels && session_.me() >= 0 && session_.room().panel(proto::PanelId(session_.me(), number));
}

// After anything that may have changed who watches one of our panels: its source is told
// whether anyone does and how often to encode for the one who sees most of it, and asked for a
// keyframe when someone waits for one.
void Video::Settle(int number, double now) {
    Shared &s = shared_[number];
    const int pictures = PicturesFor(s.viewers.level(), kBalancedPictures);
    if (pictures > 0 && pictures != s.pictures) programs_.Pictures(number, s.pictures = pictures);
    if (s.viewers.watched() != s.watched) programs_.Watched(number, s.watched = s.viewers.watched());
    if (s.viewers.WantsKey(now)) programs_.Key(number);
}

void Video::SourceCame(int number) {
    if (number < 0 || number >= room::kFramePanels) return;
    Shared &s = shared_[number];
    programs_.Rate(number, s.pace.bitrate());
    programs_.Pictures(number, s.pictures);
    programs_.Watched(number, s.watched = s.viewers.watched());
}

void Video::Unit(int number, bool key, const uint8_t *unit, size_t n, double now) {
    if (!Mine(number) || !unit || n == 0) return;
    Shared &s = shared_[number];
    proto::FrameHead head;
    head.panel = proto::PanelId(session_.me(), number), head.number = ++s.count, head.key = key;
    const std::vector<uint8_t> bytes = proto::PackFrame(head, unit, n);
    if (bytes.empty()) {   // longer than any picture may be: nobody gets it, so what leans on it is no use to anyone
        for (int link : s.viewers.For(false)) s.viewers.GaveUp(link);
        return Settle(number, now);
    }
    for (int link : s.viewers.For(key)) {
        if (session_.PersonOn(link) < 0) s.viewers.Gone(link);
        else if (!wires_.Frame(link, bytes, uint32_t(number))) Lost(link, uint32_t(number), now);
    }
    Settle(number, now);
}

void Video::Watch(int link, const proto::Watch &watch, double now) {
    const int number = watch.panel % proto::kPanelNumbers;
    if (watch.panel / proto::kPanelNumbers != session_.me() || !Mine(number)) return;
    shared_[number].viewers.Watch(link, watch.level, watch.fresh);
    Settle(number, now);
}

void Video::Lost(int link, uint32_t tag, double now) {
    if (tag >= uint32_t(room::kFramePanels)) return;
    Shared &s = shared_[tag];
    s.viewers.GaveUp(link);
    if (s.pace.GaveUp(now)) programs_.Rate(int(tag), s.pace.bitrate());
}

void Video::Frame(int link, const uint8_t *data, size_t n, double now) {
    proto::FrameHead head;
    const uint8_t *unit = nullptr;
    size_t size = 0;
    const int person = session_.PersonOn(link);
    const auto it = person >= 0 && proto::ReadFrame(data, n, head, unit, size) ? watching_.find(head.panel) : watching_.end();
    // a picture is its sender's own panel's, or it is nothing
    if (it == watching_.end() || head.panel / proto::kPanelNumbers != person || !session_.room().panel(head.panel) || !it->second.allowance.Spend(now)) {
        ++dropped_;
        return;
    }
    Order::Frame frame;
    frame.number = head.number, frame.key = head.key, frame.unit.assign(unit, unit + size);
    std::vector<Order::Frame> pass;
    it->second.order.Take(std::move(frame), now, pass);
    for (const Order::Frame &f : pass) programs_.Show(head.panel, f.key, f.unit);
}

// After anything that may have changed which of the panels shown here decode: their programs
// are told. One that is to decode begins at a keyframe, and nothing more is shown of a still.
void Video::Decoders(double now) {
    for (const auto &[panel, w] : watching_) budget_.Sees(panel, w.seen.Told(now), now);
    for (const Budget::Word &word : budget_.Settle(now)) {
        programs_.Decode(word.panel, word.decode);
        Watching &w = watching_[word.panel];
        w.fresh = true, w.order.Reset();
    }
}

// What a panel's owner is to hear its viewer here sees of it: nothing while it is a still.
int Video::Told(uint8_t panel, const Watching &w, double now) const { return budget_.decodes(panel) ? w.seen.Told(now) : int(proto::kHidden); }

void Video::Tell(uint8_t panel, Watching &w, double now) {
    proto::Watch watch;
    watch.panel = panel, watch.level = uint8_t(Told(panel, w, now)), watch.fresh = w.fresh;
    if (!session_.Watch(watch).empty()) return;   // no link to its owner yet: said again at the next Tick
    w.fresh = false, w.told = now, w.said = watch.level;
}

void Video::SinkCame(uint8_t panel, double now) {
    Watching &w = watching_[panel] = Watching{};
    const proto::Panel *shown = session_.room().panel(panel);
    budget_.Came(panel, shown ? Budget::Load(shown->width, shown->height, kBalancedPictures) : 0);
    Decoders(now);
    Tell(panel, w, now);
}

void Video::SinkWent(uint8_t panel) {
    if (!watching_.erase(panel)) return;
    budget_.Went(panel);
    proto::Watch watch;
    watch.panel = panel, watch.level = proto::kHidden;
    session_.Watch(watch);
}

void Video::Attention(uint8_t panel, int level, bool fresh, double now) {
    const auto it = watching_.find(panel);
    if (it == watching_.end() || level < proto::kHidden || level > proto::kFocused) return;
    Watching &w = it->second;
    if (fresh) w.fresh = true, w.order.Reset();
    w.seen.Set(level, now);
    Decoders(now);
    Tell(panel, w, now);
}

void Video::SinkDecodes(uint8_t panel, bool has, double now) {
    budget_.Has(panel, has);
    Decoders(now);
}

void Video::SinkRefused(uint8_t panel, double now) {
    budget_.Refused(panel, now);
    Decoders(now);
}

std::vector<uint8_t> Video::stills() const {
    std::vector<uint8_t> out;
    for (const auto &[panel, w] : watching_)
        if (!budget_.decodes(panel)) out.push_back(panel);
    return out;
}

void Video::Tick(double now) {
    Decoders(now);
    for (auto &[panel, w] : watching_) {
        if (w.order.WantsKey(now)) w.fresh = true;
        if (w.fresh || now - w.told >= kTellEvery || Told(panel, w, now) != w.said) Tell(panel, w, now);
    }
    for (int number = 0; number < room::kFramePanels; ++number) {
        Shared &s = shared_[number];
        if (!Mine(number)) s.viewers = Viewers{};   // not shared any more, or the room was left
        else s.viewers.Keep([this](int link) { return session_.PersonOn(link) >= 0; });
        Settle(number, now);
        if (!s.viewers.watched()) s.pace.Idle(now);
        else if (s.pace.Tick(now)) programs_.Rate(number, s.pace.bitrate());
    }
}

}  // namespace flow
