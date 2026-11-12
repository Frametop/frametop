// What the wearer lets the video of their panels have, behind lines of text: the questions on
// ft-together's socket that begin with `quality` (together/README.md, "A shared panel's video";
// flow/quality.h has the names and the numbers).
//
//   quality                     -> ok NAME: low, balanced or high
//   quality low|balanced|high   -> from now on, for every panel of theirs: ok NAME
//
// It holds until ft-together ends. What it starts with is frametop.conf's TOGETHER_QUALITY
// (session/frametop.conf.example), which the Together page writes when its person picks one.
//
// Header only.
#pragma once

#include "../../conf/ft_conf.h"
#include "../flow/video.h"

#include <string>

namespace local {

// Whether `asked` is one of them, and then its answer in `reply`.
inline bool QualityAsk(flow::Video &video, const std::string &asked, std::string &reply) {
    if (asked != "quality" && asked.rfind("quality ", 0) != 0) return false;
    if (asked.size() > 7) {
        const flow::Quality *quality = flow::QualityNamed(asked.substr(8));
        if (!quality) return reply = "error quality low|balanced|high", true;
        video.SetQuality(*quality);
    }
    reply = std::string("ok ") + video.quality().name;
    return true;
}

// What frametop.conf has (FT_TOGETHER_QUALITY in the environment before it), or Balanced when
// it has none or one that isn't a name.
inline const flow::Quality &QualityKept() {
    const flow::Quality *quality = flow::QualityNamed(ft_conf::Setting("TOGETHER_QUALITY", flow::kBalanced.name));
    return quality ? *quality : flow::kBalanced;
}

}  // namespace local
