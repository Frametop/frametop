// The grab bar menu's icons, as the Option C design draws them (its OptionC, States and Vocabulary
// boards; user, 2026-11-19: "Use that as the design"). Each is the design's own SVG on its 24-unit
// viewBox (iconpath.h draws it).
#pragma once

#include "iconpath.h"

#include <vector>

namespace barmenuicons {

enum class Icon {
    None, Curved, Rotate, Desktop, Close, Theater, InRoom, InView, Dashboard, Controller, StayShown, Centre, MoveAll,
    BarPosition, Reset, Back, Chevron, Check, Scale,
};

inline std::vector<iconpath::Shape> Shapes(Icon i) {
    using iconpath::Ellipse;
    using iconpath::Parse;
    using iconpath::Rect;
    using iconpath::Shape;
    switch (i) {
        case Icon::Curved: return {{Parse("M3 5c5.6 2.6 12.4 2.6 18 0v13c-5.6-2.6-12.4-2.6-18 0z")}};
        case Icon::Rotate: return {{{iconpath::Turned(Rect(4, 5, 16, 11, 2), -16, 12, 11), Rect(3, 20, 18, 2, 1)}}};
        case Icon::Desktop: return {{Parse("M11 3h2v9l3-3 1.4 1.4L12 15.8l-5.4-5.4L8 9l3 3z")}, {{Rect(5, 18, 14, 2)}}};
        case Icon::Close:
            return {{Parse("M6.4 4.6 12 10.2l5.6-5.6 1.8 1.8L13.8 12l5.6 5.6-1.8 1.8L12 13.8l-5.6 5.6-1.8-1.8L10.2 12 4.6 6.4z")}};
        case Icon::Theater:
            return {{Parse("M3 4c6-1.3 12-1.3 18 0v11c-6-1.3-12-1.3-18 0z")}, {{Ellipse(8, 19.5, 2, 2), Ellipse(16, 19.5, 2, 2)}}};
        case Icon::InRoom: return {{{Rect(4, 3, 16, 11, 1.5)}}, {{Ellipse(12, 20, 7, 1.8)}, false, 0.55}};
        case Icon::InView: return {{Parse("M3 8h18a1 1 0 0 1 1 1v6a2 2 0 0 1-2 2h-4.2l-2-2.6h-3.6l-2 2.6H4a2 2 0 0 1-2-2V9a1 1 0 0 1 1-1z")}};
        case Icon::Dashboard: return {{{Rect(5, 3, 14, 10, 1.5), Rect(2, 16, 20, 5, 1.5)}}};
        case Icon::Controller:
            return {{Parse("M9 2h6a3 3 0 0 1 3 3v3.5a3 3 0 0 1-2 2.8V20a2 2 0 0 1-2 2h-4a2 2 0 0 1-2-2v-8.7a3 3 0 0 1-2-2.8V5a3 3 0 0 1 3-3z")}};
        case Icon::StayShown:
            return {{Parse("M14 3l7 7-2 1-3-1-3 3 1 4-1.5 1.5L8 14l-4.5 5.5-1-1L8 13 4.5 9.5 6 8l4 1 3-3-1-3z")}};
        // (not the design's: a big screen between two narrow ones, in its style)
        case Icon::Centre: return {{{Rect(6.5, 5, 11, 12, 1.5)}}, {{Rect(1.5, 8, 3.5, 6, 1), Rect(19, 8, 3.5, 6, 1)}, false, 0.55}};
        case Icon::MoveAll:
            return {{Parse("M12 1l3.2 3.2H13V8h-2V4.2H8.8zM12 23l-3.2-3.2H11V16h2v3.8h2.2zM1 12l3.2-3.2V11H8v2H4.2v2.2zM23 12l-3.2 3.2V13H16v-2h3.8V8.8z")},
                    {{Rect(9.5, 9.5, 5, 5, 1)}}};
        case Icon::BarPosition: return {{Parse("M3 3h18v18H3zm2 2v10h14V5z"), true}, {{Rect(7, 17, 10, 2, 1)}}};
        case Icon::Reset: return {{Parse("M12 4a8 8 0 1 1-7.4 5h2.2A6 6 0 1 0 12 6v3L7.5 5 12 1z")}, {{Rect(9, 10, 6, 5, 1)}}};
        case Icon::Back: return {{Parse("M15 5l-7 7 7 7 1.4-1.4L10.8 12l5.6-5.6z")}};
        case Icon::Chevron: return {{Parse("M9 5l7 7-7 7-1.4-1.4 5.6-5.6-5.6-5.6z")}};
        case Icon::Check: return {{Parse("M9.5 16.2 5.3 12l-1.4 1.4 5.6 5.6L21 7.5l-1.4-1.4z")}};
        // (not the design's: a small and a big letter A, in its style)
        case Icon::Scale:
            return {{Parse("M14 4h2.4L22 20h-2.5l-1.4-4.2h-5.8L10.9 20H8.4zM15.2 7l-2.2 6.6h4.4z"), true},
                    {Parse("M4.4 11h1.7L9.4 20H7.6l-.8-2.4H3.7L2.9 20H1.1zM5.2 13l-1.1 3.2h2.3z"), true}};
        case Icon::None: break;
    }
    return {};
}

}  // namespace barmenuicons
