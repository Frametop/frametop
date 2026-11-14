// A picture of someone in a shared room with no headset on and no room: the abstract body of a
// place, or an avatar they sent (sentbody.h), posed as a demo wearer whose pose went through
// everything a real one's does (told in the room's directions with the mouth their voice
// made, packed, read, and fitted on another floor), and drawn on their sheet for a pair of
// eyes standing in front of them, both eyes' views side by side as SteamVR gets them, clear
// where no body is. To check by eye what people in a room look like.
#pragma once

#include "../bodyfile.h"

#include <string>

namespace roompicture {

struct Ask {
    std::string out;
    int place = 0;               // whose colour
    std::string model;           // the file of the avatar they sent ("": none, the abstract body)
    std::string pose = "wave";   // demo's names
    double at = 1;               // seconds into the pose
    float shapes[bodyfile::Shape::kLastId + 1] = {};   // their mouth as their pose tells it: the five vowels' weights, by Shape::Id
    double away = 1.5;           // the eyes stand this far from them, metres
    double round = 0;            // ...and this far round them from straight in front, degrees
    int size = 512;              // an eye's view, pixels a side
};

// The program's exit status, with a line on stderr when it isn't 0.
int Person(const Ask &ask);

}  // namespace roompicture
