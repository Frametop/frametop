// The face of the avatar you wear yourself: its mouth moves with your voice, as your microphone
// hears it (micear.h), and its eyes blink (face.h). Nothing of the voice is kept or sent: it is
// boiled down to the mouth's five weights as it comes.
#pragma once

#include "face.h"
#include "micear.h"

namespace ownface {

class Face {
public:
    // Starts hearing the microphone, as `name` in the mixer. With none the mouth stays shut and
    // the eyes still blink.
    void Listen(const char *name) { ear_.Open(name); }
    // The face `dt` seconds on from the call before, into the weights (by Shape::Id).
    // `hasBlink`: the body has the shape that shuts both eyes (face::Lids).
    void After(double dt, bool hasBlink, float weights[face::kWeights]);
    bool hearing() const { return ear_.hearing(); }
    const face::Mouth &mouth() const { return mouth_; }

private:
    micear::Ear ear_;
    face::Mouth mouth_;
    face::Blink blink_;
};

// `ft-avatars --hear SECONDS`: what the mouth makes of the microphone, a line four times a
// second (how loud, the room's noise, the five weights). For checking a microphone and tuning
// by eye; 0 when it heard one.
int Hear(double seconds);

}  // namespace ownface
