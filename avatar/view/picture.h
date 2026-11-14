// ft-avatars' pictures of a body with no headset on: a model file's body posed as a demo wearer
// (demo.h) and drawn offscreen into a PNG. A sheet shows it from four sides, to check an
// import by eye; a thumb is the one square picture Settings shows for it; a glass is what the
// mirror (mirror.h) shows the wearer standing in front of it, both eyes' views side by side as
// SteamVR gets them; and a below is what the wearer is shown of their own body under them
// (below.h), the same way, clear where no body is.
#pragma once

#include "bodymesh.h"

#include <string>

namespace picture {

struct Ask {
    std::string model, out;
    std::string pose = "wave";   // demo's names, or "rest": as the file has it, unposed
    double at = 1;               // seconds into the pose
    int size = 720;              // a view's height (a thumb's side, an eye's view of the glass), pixels
    float shapes[bodymesh::kShapes] = {};
};

// Each returns the program's exit status, with a line on stderr when it isn't 0.
int Sheet(const Ask &ask);
int Thumb(const Ask &ask);
int Glass(const Ask &ask);
int Below(const Ask &ask);

}  // namespace picture
