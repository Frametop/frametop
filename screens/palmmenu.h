// The palm menu (frame-hands/notes/hand-interaction.md, section 11): turn a bare hand's palm up
// toward your face and look at it, and after a moment Quick Access shows just above the palm,
// following it: a card of tiles (quickaccess.h, drawn by vrquick.cpp) the other hand's fingertip
// or a laser presses (user, 2026-11-18: "when someone looks at their palm, they can then tap or use
// a laser with their other hand to do quick actions"; until then a small ring, like the Vision
// Pro's home indicator). A thumb-index pinch tap by that hand while it shows still opens the Steam
// menu (`ft-steam menu`, the input relay's steam_menu), or closes the SteamVR dashboard if it's
// open; its Steam Menu tile lights while the pinch is held, and the release does it, or, dragged
// to another tile while it's held, that one (quickaccess.h's Drag). Turning the
// hand away hides it, unless the other hand or a laser is on the card (Input::engaged).
//
// While it shows, that hand is ours: vr.cpp reports it near ("near ... <L> <R>" to the
// pointer helper), so the helper leaves its pinches alone (no gaze click), and near input
// (handinput.h) is off for it. A pinch that began before it showed stays the helper's.
//
// Pure logic, like handinput.cpp: no OpenVR or GL calls (tests: screens/tests/handinput_test.cpp).
#pragma once

#include "handcut.h"

#include <cstdint>
#include <string>
#include <vector>

namespace palmmenu {

using Mat = handcut::Mat;

struct Params {
    bool on = true;                 // HANDS_PALM_MENU
    // The palm faces the eyes: its normal (out of the palm) within this of the line from the palm
    // to the head. A flat palm up at chest height, 0.3 m under the eyes and 0.3 m ahead, is 45 deg
    // off; held up at eye height it's 90. The hand ray needs the palm turned away (fh_ray.h), so
    // the two never meet.
    // 45 since the third live test (2026-10-10: the menu opened as the user took a panel's bottom
    // edge from below, palm up, fingers bunched; 60 and 75 before).
    double facing = 45, facingLeave = 60;     // degrees
    // ...and you look at it: the palm within this of the head's forward direction
    double look = 45, lookLeave = 55;         // degrees
    // ...with the hand open: the middle, ring and little fingers' extension (tip-to-wrist over
    // knuckle-to-wrist, as handinput's), averaged. Straight is about 2, a fist under 1. The index
    // and thumb don't count: they pinch.
    double open = 1.5, openLeave = 1.3;
    double nearest = 0.15, farthest = 0.70;   // the palm this far from the head (m)
    double showMs = 300;            // held this long (and still: the palm under `still` m/s) before it shows
    double still = 0.25;
    // Not by a panel, nor busy (user, 2026-10-10: "make the palm menu much stricter"): not shown while
    // the hand is at a panel (Input::atPanel: in or near its touch zone, or within 12 cm in front
    // of one and 8 cm round it) or near input or "grab that" has it (Input::busy), nor for quietMs
    // after.
    double quietMs = 1000;
    // The pinch: a plain thumb-index one, the middle finger out of it (its tip plainMiddle or more
    // from the thumb's, the finger plainExtended or straighter: not a card's index and middle, nor
    // the bunched fingertips), the palm still (under pinchStill m/s) as it begins. Another pinch
    // while it shows does nothing.
    double plainMiddle = 0.045, plainExtended = 1.35, pinchStill = 0.35;
    double hideMs = 150;            // ...and away this long before it hides
    double lostMs = 250;            // a hand unseen this long: it goes, a pinch is cancelled
    double againMs = 500;           // no second menu toggle this soon (a pinch read twice)
    // The card: its centre this far out of the palm's centre, toward the eyes, clear of the fingers
    // (m), and this wide: its six tiles about 3.5 cm, a fingertip's poke (quickaccess.h).
    double lift = 0.08;
    double size = 0.12;
};

// What the palm-up test saw (for the log and tests).
struct Pose {
    double facingDeg = 180, lookDeg = 180, open = 0, dist = 0;
};

// The palm's normal, out of the palm (the side you see in your palm), from the wrist and the
// index and little knuckles; the left hand's is turned round (handedness, as the pointer helper's
// handsin.h).
void PalmNormal(const handcut::Hand &h, double out[3]);
// The palm faces the head and the head looks at it, with the hand open. was: it did last frame
// (the leave thresholds apply).
bool PalmUp(const handcut::Hand &h, const Mat &head, const Params &k, bool was, Pose *pose = nullptr);

struct Input {
    int64_t nowNs = 0;
    const handcut::Hands *hands = nullptr;
    Mat head{};
    bool haveHead = false;
    bool allowed[2] = {false, false};   // per side: a bare hand, and hand input allowed now
    bool busy[2] = {false, false};      // near input presses or holds a panel with that hand, or "grab that" has it
    bool atPanel[2] = {false, false};   // the hand is at a panel (handinput's near(), or HandAtPanel)
    bool engaged[2] = {false, false};   // that side's card has the other hand's fingertip or a laser on it
};

class PalmMenu {
public:
    explicit PalmMenu(const Params &p = {}) : p_(p) {}
    Params &params() { return p_; }
    const Params &params() const { return p_; }

    // Once per tick, after the hands were read, before near input.
    void Update(const Input &in);
    // It shows on that side (the hand is ours: its pinches aren't the helper's).
    bool shown(int side) const { return side_[side].state == State::Shown || side_[side].state == State::Pinched; }
    bool anyShown() const { return shown(0) || shown(1); }
    // ...and that hand's pinch is held (Quick Access's Drag: what it lights, its release does).
    bool pinched(int side) const { return side_[side].state == State::Pinched; }
    // That palm passed the palm-up test on the last frame (a pinch let go otherwise does nothing).
    bool up(int side) const { return side_[side].up; }
    // ...and how many pinches have been held on that side: a new one since (council, 2026-11-19: let go
    // and pinched again in one read, toggled or not) starts its drag afresh.
    uint32_t pinchCount(int side) const { return side_[side].pinches; }
    // This Update's pinch released while it showed: do what it lit on the card (vrquick.cpp's
    // UpdateQuickAccess: Steam Menu unless it was dragged; the side that did it, or -1).
    int toggled() const { return toggled_; }
    // Where the card's centre goes for this side's hand in `hands` (Hands::shown(), moved ahead to now).
    bool Card(int side, const std::vector<handcut::Hand> &hands, double out[3]) const;
    // "left shown, right idle"
    std::string Describe() const;

    struct Stats {
        int shows = 0, pinches = 0, toggles = 0, cancels = 0;
        int notPlain = 0;   // pinches while it showed that weren't a plain thumb-index one (or not still)
    } stats;

private:
    enum class State { Off, Idle, Shown, Pinched };
    struct Side {
        State state = State::Idle;
        uint32_t handId = 0;
        bool up = false;                     // the palm-up test passed on the last frame
        int64_t upSince = -1, awaySince = -1, lastSeen = 0;
        int64_t shownNs = 0;                 // the capture time of the frame it showed on
        int64_t quietUntil = 0;              // not shown before this (Params::quietMs)
        uint32_t begins = 0;                 // the pinch slot's begins counter last seen
        bool knowBegins = false;
        uint32_t pinches = 0;                // pinches held on it (Pinched entered), ever
    };
    const handcut::Hand *HandFor(int side, const std::vector<handcut::Hand> &hands) const;
    // A plain thumb-index pinch by a still hand (Params::plainMiddle).
    bool Plain(const handcut::Hand *h) const;
    void Release(int s, bool lost, int64_t now);

    Params p_;
    Side side_[2];
    uint64_t frame_ = 0;
    int64_t lastToggle_ = INT64_MIN / 2;
    int toggled_ = -1;
};

}  // namespace palmmenu
