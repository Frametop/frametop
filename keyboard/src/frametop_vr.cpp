#include "framekeyboard/frametop_vr.hpp"
#include "framekeyboard/app.hpp"

namespace framekeyboard {
bool AppPointers::move(unsigned pointer, double x, double y) { return app_.move(pointer, x, y); }
bool AppPointers::down(unsigned pointer, double x, double y, double now) {
    return app_.down(pointer, x, y, now);
}
bool AppPointers::up(unsigned pointer, double x, double y) { return app_.up(pointer, x, y); }
void AppPointers::cancel_pointer(unsigned pointer) { app_.cancel_pointer(pointer); }

FrametopVr::FrametopVr(FrametopLink* link, PointerSink& pointers, FrametopVrAccess access)
    : link_(link), pointers_(pointers), access_(std::move(access)) {}

void FrametopVr::place(PanelPlacement& placement) const {
    if (link_) {
        // Where Frametop's old keyboard opened, 0.70 m ahead and 0.35 m under the eyes, facing
        // them (26.6 degrees). A hand's laser aimed 65 cm down lowers the hand past where
        // Frametop puts the laser away.
        placement.set_head_spot(.70, .35, 26.6);
    }
}

bool FrametopVr::begin_frame(bool relaunch, bool visible) {
    if (!link_) {
        return relaunch;
    }
    link_->poll();
    auto request = link_->take_request();
    if (request == FrametopLink::Request::Toggle) {
        request = open_ && visible ? FrametopLink::Request::Hide : FrametopLink::Request::Show;
    }
    if (request == FrametopLink::Request::Show) {
        // A text field that gets focus again while it's up doesn't move it.
        relaunch |= !(open_ && visible);
        open_ = true;
    } else if (request == FrametopLink::Request::Hide) {
        open_ = false;
    }
    open_ |= relaunch;
    return relaunch;
}

bool FrametopVr::surface(bool dashboard, bool steam_field) const {
    if (!link_) {
        return dashboard;
    }
    return (dashboard || link_->surface()) && (open_ || steam_field);
}

void FrametopVr::shown(bool visible) {
    if (link_) {
        link_->set_open(open_ && visible);
    }
}

// Each Frame controller's "tip" component, the nose of its body (within 4 mm along its aim in
// the render model), read once per device and tried again at most once a second.
std::optional<std::array<double, 3>> FrametopVr::tip(std::size_t hand, double now) {
    const auto device = access_.hand_device(hand);
    if (device != devices_[hand]) {
        // A new device: none this frame, so a press on the old one ends.
        devices_[hand] = device;
        tips_[hand].reset();
        tried_[hand] = -1e9;
        return {};
    }
    if (!tips_[hand] && now - tried_[hand] > 1) {
        tried_[hand] = now;
        tips_[hand] = device == FrametopVrAccess::no_device ? std::nullopt
                                                            : access_.tip_component(device, hand == 0);
    }
    const auto pose = tips_[hand] ? access_.pose(device) : std::nullopt;
    if (!pose) {
        return {};
    }
    return tip_point(*pose, *tips_[hand], tip_reach);
}

bool FrametopVr::touch(bool touching, const PanelPlacement& displayed,
                       const std::function<Rect()>& case_bounds,
                       const std::function<bool(unsigned)>& gripping, KeyHaptics& feedback, double now) {
    if (!touching && !tips_near()) {
        return false;
    }
    std::optional<Rect> bounds;
    for (std::size_t hand = 0; hand < pokes_.size(); ++hand) {
        std::optional<PanelPoint> point;
        const auto at = touching ? tip(hand, now) : std::nullopt;
        const auto device = devices_[hand];
        if (at && !gripping(device)) {
            point = locate_on_panel(displayed.transform(), displayed.width(), *at);
        }
        if (!bounds) {
            bounds = case_bounds();
        }
        const auto pointer = touch_pointer + static_cast<unsigned>(hand);
        const auto event = pokes_[hand].update(point, *bounds, now);
        switch (event.kind) {
        case TouchEvent::Kind::Hover:
            if (pointers_.move(pointer, event.x, event.y)) {
                feedback.hover(device);
            }
            break;
        case TouchEvent::Kind::Move:
            pointers_.move(pointer, event.x, event.y);
            break;
        case TouchEvent::Kind::Down:
            if (pointers_.down(pointer, event.x, event.y, now)) {
                feedback.press(pointer, device);
            }
            break;
        case TouchEvent::Kind::Up:
            if (pointers_.up(pointer, event.x, event.y)) {
                feedback.release(pointer);
            }
            break;
        case TouchEvent::Kind::Leave:
        case TouchEvent::Kind::Cancel:
            pointers_.cancel_pointer(pointer);
            feedback.cancel_pointer(pointer);
            break;
        case TouchEvent::Kind::None:
            break;
        }
    }
    return true;
}

void FrametopVr::end_hand(std::size_t hand) {
    if (hands_[hand].active) {
        pointers_.cancel_pointer(hand_pointer + static_cast<unsigned>(hand));
    }
    hands_[hand] = {};
}

// Its point lands where it is on the panel as shown now. No haptics: there's no controller.
bool FrametopVr::hand_pokes(bool takes, const PanelPlacement& displayed, double now) {
    if (!link_) {
        return false;
    }
    takes = takes && link_->fresh();
    for (const auto& poke : link_->take_pokes()) {
        auto& hand = hands_[poke.hand];
        const auto pointer = hand_pointer + static_cast<unsigned>(poke.hand);
        if (!takes || poke.verb == HandPoke::Verb::End) {
            end_hand(poke.hand);
            continue;
        }
        if (poke.point) {
            const auto at = locate_on_panel(displayed.transform(), displayed.width(), *poke.point);
            hand.x = at.x, hand.y = at.y;
        }
        hand.active = true, hand.heard = now;
        switch (poke.verb) {
        case HandPoke::Verb::Hover:
        case HandPoke::Verb::Move:
            pointers_.move(pointer, hand.x, hand.y);
            break;
        case HandPoke::Verb::Press:
            if (hand.pressed) {
                pointers_.up(pointer, hand.x, hand.y);
            }
            // (false for a button, which still wants its up)
            pointers_.down(pointer, hand.x, hand.y, now);
            hand.pressed = true;
            break;
        case HandPoke::Verb::Up:
            if (hand.pressed) {
                pointers_.up(pointer, hand.x, hand.y);
            }
            hand.pressed = false;
            break;
        case HandPoke::Verb::End:
            break;
        }
    }
    for (std::size_t hand = 0; hand < hands_.size(); ++hand) {
        if (hands_[hand].active && (!takes || (hands_[hand].pressed && now - hands_[hand].heard > hand_silence))) {
            end_hand(hand);
        }
    }
    return true;
}
} // namespace framekeyboard
