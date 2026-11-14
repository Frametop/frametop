// The shared panels' video behind lines of text: the questions on ft-together's socket about
// who makes the pictures and how they go (together/README.md, "A shared panel's video").
//
//   media    -> ok {"sources":[numbers],"sinks":[panel ids],"sender":true|false,"bitrate":[...],
//               "pictures":[...],"viewers":[...],"stills":[panel ids],"dropped":N}: which of the
//               wearer's panels have a program that encodes them here and which of the others'
//               one that shows them, whether the wearer's PC is here to send its own, what each
//               of the wearer's panels (by its number: the Frame's, then the PC's) is encoded at
//               and how many watch it, which panels shown here are stills, and how many pictures
//               from the links and from the PC weren't taken
//   sender   -> ok CODE: a code for the wearer's split compute PC to call this Frame with, good
//               once, for ten minutes (flow/pcpanels.h). Whoever asks hands it to the PC
//
// Header only.
#pragma once

#include "../flow/pcpanels.h"
#include "media.h"

#include <string>

namespace local {

inline std::string MediaState(const flow::Video &video, const flow::PcPanels &pc, const Media &media) {
    std::string state = media.State(), rates, pictures, viewers, stills;
    for (const uint8_t panel : video.stills()) stills += (stills.empty() ? "" : ",") + std::to_string(int(panel));
    for (int number = 0; number < room::kOwnPanels; ++number) {
        rates += (number ? "," : "") + std::to_string(video.bitrate(number));
        pictures += (number ? "," : "") + std::to_string(video.pictures(number));
        viewers += (number ? "," : "") + std::to_string(video.viewers(number));
    }
    state.pop_back();
    return state + ",\"sender\":" + (pc.linked() ? "true" : "false") + ",\"bitrate\":[" + rates + "],\"pictures\":[" + pictures + "],\"viewers\":[" + viewers + "],\"stills\":[" + stills +
           "],\"dropped\":" + std::to_string(video.dropped() + pc.dropped()) + "}";
}

// Whether `asked` is one of them, and then its answer in `reply`.
inline bool MediaAsk(const flow::Video &video, flow::PcPanels &pc, const Media &media, const std::string &asked, double now, std::string &reply) {
    if (asked == "media") reply = "ok " + MediaState(video, pc, media);
    else if (asked == "sender") reply = "ok " + pc.Invite(now);
    else return false;
    return true;
}

}  // namespace local
