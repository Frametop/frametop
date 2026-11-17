// gsforward.h's X connection, on xcb (wlroots' Xwayland support brings it into both build
// containers; Xlib's headers are in the dev one only).
#include "gsforward.h"

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xtest.h>

#include <cstdlib>
#include <cstring>
#include <map>

namespace gsforward {

struct Link::Impl {
    xcb_connection_t *c = nullptr;
    xcb_window_t root = 0;
    uint32_t pid = 0;
    Role role = Role::Apps;
    xcb_atom_t target = 0, steamGame = 0, netName = 0, utf8 = 0;
    std::map<uint32_t, bool> top;   // root's children seen, and whether each is override-redirect
    std::vector<Event> pending;

    xcb_atom_t Atom(const char *name) {
        auto r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, uint16_t(std::strlen(name)), name), nullptr);
        const xcb_atom_t a = r ? r->atom : xcb_atom_t(XCB_ATOM_NONE);
        std::free(r);
        return a;
    }
    uint32_t Cardinal(xcb_window_t w, xcb_atom_t a, bool *has = nullptr) {
        auto r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, a, XCB_ATOM_CARDINAL, 0, 1), nullptr);
        uint32_t v = 0;
        const bool there = r && xcb_get_property_value_length(r) >= 4;
        if (there) std::memcpy(&v, xcb_get_property_value(r), 4);
        if (has) *has = there;
        std::free(r);
        return v;
    }
    // The overlay a window's target names (its two halves), 0 for none.
    uint64_t Target(xcb_window_t w) {
        auto r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, target, XCB_ATOM_CARDINAL, 0, 2), nullptr);
        uint64_t v = 0;
        if (r && xcb_get_property_value_length(r) >= 8) {
            const uint32_t *h = static_cast<const uint32_t *>(xcb_get_property_value(r));
            v = uint64_t(h[0]) | uint64_t(h[1]) << 32;
        }
        std::free(r);
        return v;
    }
    std::string Text(xcb_window_t w, xcb_atom_t a, xcb_atom_t type) {
        auto r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, a, type, 0, 256), nullptr);
        std::string s;
        if (r) s.assign(static_cast<const char *>(xcb_get_property_value(r)), size_t(xcb_get_property_value_length(r)));
        std::free(r);
        return s;
    }
    // A child of the root as a Window, classified; false if it went meanwhile.
    bool Read(xcb_window_t id, Window *w, bool *mapped) {
        auto a = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, id), nullptr);
        auto g = xcb_get_geometry_reply(c, xcb_get_geometry(c, id), nullptr);
        bool ok = a && g;
        if (ok) {
            xcb_window_t tf = 0;
            xcb_icccm_get_wm_transient_for_reply(c, xcb_icccm_get_wm_transient_for(c, id), &tf, nullptr);
            w->id = id, w->x = g->x, w->y = g->y, w->w = g->width, w->h = g->height, w->transientFor = tf;
            w->overrideRedirect = a->override_redirect != 0;
            w->kind = Classify(a->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT, a->override_redirect, tf != 0, w->w, w->h);
            top[id] = a->override_redirect;
            if (mapped) *mapped = a->map_state == XCB_MAP_STATE_VIEWABLE;
            if (role == Role::Steam) {   // (its target now, and each change of it from here on)
                const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
                xcb_change_window_attributes(c, id, XCB_CW_EVENT_MASK, &mask);
                w->target = Target(id);
            }
        }
        std::free(a), std::free(g);
        return ok;
    }
};

Link::Link() : d_(new Impl) {}
Link::~Link() { Close(); }

bool Link::IsOpen() const { return d_->c != nullptr; }
int Link::Fd() const { return d_->c ? xcb_get_file_descriptor(d_->c) : -1; }
uint32_t Link::GamescopePid() const { return d_->pid; }

bool Link::Open(const char *display, bool needForwarding, std::string *why, Role role) {
    const bool steams = role == Role::Steam;
    needForwarding = needForwarding || steams;
    Close();
    int screen = 0;
    xcb_connection_t *c = xcb_connect(display, &screen);
    if (!c || xcb_connection_has_error(c)) {
        if (c) xcb_disconnect(c);
        if (why) *why = std::string("no X display ") + display;
        return false;
    }
    Impl &d = *d_;
    d.c = c;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen && it.rem; ++i) xcb_screen_next(&it);
    d.root = it.data->root;
    const xcb_query_extension_reply_t *xtest = xcb_get_extension_data(c, &xcb_test_id);
    if (!xtest || !xtest->present) {
        if (why) *why = std::string(display) + " has no XTest";
        Close();
        return false;
    }
    const uint32_t forwarding = d.Cardinal(d.root, d.Atom("GAMESCOPE_VROVERLAY_FORWARDING"));
    d.pid = d.Cardinal(d.root, d.Atom("GAMESCOPE_PID"));
    if (needForwarding && (!forwarding || !d.pid)) {
        if (why) *why = std::string(display) + " isn't a gamescope that forwards windows to overlays";
        Close();
        return false;
    }
    bool numbered = false;   // (gamescope's first Xwayland is Steam's own, not its apps')
    const uint32_t server = d.Cardinal(d.root, d.Atom("GAMESCOPE_XWAYLAND_SERVER_ID"), &numbered);
    if (!steams && needForwarding && numbered && server == 0) {
        if (why) *why = std::string(display) + " is Steam's own (gamescope's first Xwayland)";
        Close();
        return false;
    }
    if (steams && !(numbered && server == 0)) {
        if (why) *why = std::string(display) + " isn't Steam's own (gamescope's first Xwayland)";
        Close();
        return false;
    }
    d.role = role;
    d.target = d.Atom("STEAM_GAMESCOPE_VROVERLAY_TARGET");
    d.steamGame = d.Atom("STEAM_GAME");
    d.netName = d.Atom("_NET_WM_NAME");
    d.utf8 = d.Atom("UTF8_STRING");
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(c, d.root, XCB_CW_EVENT_MASK, &mask);
    // The windows already there (after selecting, so none is missed between).
    auto tree = xcb_query_tree_reply(c, xcb_query_tree(c, d.root), nullptr);
    if (tree) {
        const xcb_window_t *kids = xcb_query_tree_children(tree);
        for (int i = 0; i < xcb_query_tree_children_length(tree); ++i) {
            Event e;
            bool mapped = false;
            // (a tag left from an ft-screens that went without untagging names an overlay that's
            // gone: off, so the window has gamescope's own panel until one of ours tags it; Steam's
            // own windows' tags are Steam's)
            if (!steams) xcb_delete_property(c, kids[i], d.target);
            if (!d.Read(kids[i], &e.win, &mapped)) continue;
            d.pending.push_back(e);
            if (mapped) e.type = Event::Mapped, d.pending.push_back(e);
        }
        std::free(tree);
    }
    xcb_flush(c);
    return true;
}

void Link::Close() {
    if (!d_->c) return;
    xcb_disconnect(d_->c);
    d_->c = nullptr;
    d_->top.clear();
    d_->pending.clear();
}

std::vector<Event> Link::Poll() {
    Impl &d = *d_;
    std::vector<Event> out;
    out.swap(d.pending);
    if (!d.c) return out;
    while (xcb_generic_event_t *ev = xcb_poll_for_event(d.c)) {
        Event e;
        bool take = false;
        switch (ev->response_type & 0x7f) {
            case XCB_CREATE_NOTIFY: {
                auto *n = reinterpret_cast<xcb_create_notify_event_t *>(ev);
                if (n->parent == d.root && d.Read(n->window, &e.win, nullptr)) e.type = Event::Created, take = true;
                break;
            }
            case XCB_MAP_NOTIFY: {
                auto *n = reinterpret_cast<xcb_map_notify_event_t *>(ev);
                if (n->event == d.root && d.Read(n->window, &e.win, nullptr)) e.type = Event::Mapped, take = true;
                break;
            }
            case XCB_CONFIGURE_NOTIFY: {
                auto *n = reinterpret_cast<xcb_configure_notify_event_t *>(ev);
                if (n->event != d.root || !d.top.count(n->window)) break;
                e.type = Event::Moved, take = true;
                e.win.id = n->window, e.win.x = n->x, e.win.y = n->y, e.win.w = n->width, e.win.h = n->height;
                break;
            }
            case XCB_UNMAP_NOTIFY: {
                auto *n = reinterpret_cast<xcb_unmap_notify_event_t *>(ev);
                if (n->event == d.root && d.top.count(n->window)) e.type = Event::Unmapped, e.win.id = n->window, take = true;
                break;
            }
            case XCB_REPARENT_NOTIFY: {   // (an app embedding a window, or giving one back: a top-level one gone, or new)
                auto *n = reinterpret_cast<xcb_reparent_notify_event_t *>(ev);
                if (n->event != d.root) break;
                bool mapped = false;
                if (n->parent != d.root) {
                    if (d.top.erase(n->window)) e.type = Event::Gone, e.win.id = n->window, take = true;
                } else if (!d.top.count(n->window) && d.Read(n->window, &e.win, &mapped)) {
                    e.type = Event::Created, out.push_back(e);
                    if (mapped) e.type = Event::Mapped, take = true;
                }
                break;
            }
            case XCB_PROPERTY_NOTIFY: {
                auto *n = reinterpret_cast<xcb_property_notify_event_t *>(ev);
                if (d.role != Role::Steam || n->atom != d.target || !d.top.count(n->window)) break;
                e.type = Event::Retarget, e.win.id = n->window, take = true;
                e.win.target = n->state == XCB_PROPERTY_DELETE ? 0 : d.Target(n->window);
                break;
            }
            case XCB_DESTROY_NOTIFY: {
                auto *n = reinterpret_cast<xcb_destroy_notify_event_t *>(ev);
                if (n->event == d.root && d.top.erase(n->window)) e.type = Event::Gone, e.win.id = n->window, take = true;
                break;
            }
            default:
                break;   // (errors: a window that went under us, which its own events say)
        }
        std::free(ev);
        if (take) out.push_back(e);
    }
    if (xcb_connection_has_error(d.c)) Close();   // gamescope went: the caller opens again later
    return out;
}

void Link::Describe(Window *w) {
    Impl &d = *d_;
    if (!d.c) return;
    xcb_icccm_get_wm_class_reply_t cls;
    std::string klass, instance;
    if (xcb_icccm_get_wm_class_reply(d.c, xcb_icccm_get_wm_class(d.c, w->id), &cls, nullptr)) {
        instance = cls.instance_name ? cls.instance_name : "", klass = cls.class_name ? cls.class_name : "";
        xcb_icccm_get_wm_class_reply_wipe(&cls);
    }
    w->key = AppKey(d.Cardinal(w->id, d.steamGame), klass, instance);
    w->title = d.Text(w->id, d.netName, d.utf8);
    if (w->title.empty()) w->title = d.Text(w->id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING);
    if (w->title.empty()) w->title = !klass.empty() ? klass : "App";
}

void Link::Tag(uint32_t win, uint64_t overlay) {
    if (!d_->c) return;
    uint32_t halves[2];
    TargetHalves(overlay, halves);
    xcb_change_property(d_->c, XCB_PROP_MODE_REPLACE, win, d_->target, XCB_ATOM_CARDINAL, 32, 2, halves);
    xcb_flush(d_->c);
}
void Link::Untag(uint32_t win) {
    if (!d_->c) return;
    xcb_delete_property(d_->c, win, d_->target);
    xcb_flush(d_->c);
}

void Link::Pointer(int x, int y) {
    if (!d_->c) return;
    xcb_test_fake_input(d_->c, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, d_->root, int16_t(x), int16_t(y), 0);
    xcb_flush(d_->c);
}
void Link::Button(uint8_t button, bool down) {
    if (!d_->c) return;
    xcb_test_fake_input(d_->c, down ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
    xcb_flush(d_->c);
}
void Link::Scroll(int notches) {
    for (int i = 0; i < std::abs(notches); ++i) Button(notches > 0 ? 4 : 5, true), Button(notches > 0 ? 4 : 5, false);
}

void Link::Key(uint32_t win, uint8_t keycode, uint16_t state, bool down) {
    if (!d_->c || !keycode) return;
    xcb_key_press_event_t ev;
    std::memset(&ev, 0, sizeof ev);
    ev.response_type = down ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
    ev.detail = keycode;
    ev.time = XCB_CURRENT_TIME;
    ev.root = d_->root, ev.event = win, ev.child = XCB_NONE;
    ev.event_x = ev.event_y = ev.root_x = ev.root_y = 1;
    ev.state = state;
    ev.same_screen = 1;
    xcb_send_event(d_->c, 1, win, down ? XCB_EVENT_MASK_KEY_PRESS : XCB_EVENT_MASK_KEY_RELEASE,
                   reinterpret_cast<const char *>(&ev));
    xcb_flush(d_->c);
}

void Link::Raise(uint32_t win) {
    if (!d_->c) return;
    const uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(d_->c, win, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    xcb_flush(d_->c);
}
void Link::Resize(uint32_t win, int w, int h) {
    if (!d_->c || w < kMinMain || h < kMinMain) return;
    const uint32_t size[2] = {uint32_t(w), uint32_t(h)};
    xcb_configure_window(d_->c, win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    xcb_flush(d_->c);
}

}  // namespace gsforward
