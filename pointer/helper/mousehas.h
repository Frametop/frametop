// Whether the mouse has the pointer, for the relay's wake guard (input-relay.py, Pointer.motion).
// Header-only and free of OpenVR, so pointer/tests/mousehas-test.cpp can drive it.
//
// A mouse lying on a desk sends a count now and then (a knock, a sensor's flutter), and each move
// takes the pointer: it wakes it, takes SteamVR's laser from a controller, and drops a hand's ray.
// So the relay waits for POINTER_WAKE_COUNTS of motion within a second before a mouse that rested
// moves anything, and what it counted on the way is gone: 40 counts, 2.2 degrees at 0.055 a count.
// It did that after every 1.5 s rest, with the mouse the only thing in use too (user, 2026-11-24:
// "I have to move it a little bit before it actually picks up any movement ... like we have a dead
// zone"). While the mouse has the pointer there's nothing to take, so the helper says so
// ("mousehas 1", on a change and every 5 s) and the relay then moves it from the first count.
#pragma once

namespace mousehas {

// The pointer is on, and nothing else drives it: not the gaze (gaze mode aims; a move there takes the
// pointer from the eyes), not a hand's ray, not a hold a hand, a ray or the head steers.
// holdOthers: a hold is under way that the mouse didn't start.
inline bool Has(bool active, bool gazeOn, bool rayOwns, bool holdOthers) {
    return active && !gazeOn && !rayOwns && !holdOthers;
}

}  // namespace mousehas
