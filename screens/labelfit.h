// A label in a box: the lines it breaks into, between words, and the size that fits them (the palm menu's
// tiles: their words are whatever the user wrote). Pure (screens/tests/palmtiles-test.cpp): the caller
// measures.
#pragma once

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

namespace labelfit {

struct Fit {
    int size = 0;
    std::vector<std::string> lines;
};

// `s` without its last character (a whole UTF-8 one).
inline std::string Shorter(std::string s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
    return s;
}

// `s` as it is if it's no wider than `w` at `size`, else cut with "…".
template <class Width>
std::string Cut(std::string s, int size, int w, Width width) {
    if (width(s, size) <= w) return s;
    while (!s.empty() && width(s + "…", size) > w) s = Shorter(s);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + "…";
}

// The words of `text`, as many on a line as `w` pixels hold at `size`.
template <class Width>
std::vector<std::string> Wrap(const std::string &text, int size, int w, Width width) {
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        const size_t start = text.find_first_not_of(' ', at);
        if (start == std::string::npos) break;
        const size_t end = std::min(text.find(' ', start), text.size());
        const std::string word = text.substr(start, end - start);
        if (!lines.empty() && width(lines.back() + " " + word, size) <= w) lines.back() += " " + word;
        else lines.push_back(word);
        at = end;
    }
    return lines;
}

// The biggest of `sizes` (biggest first) at which `text` fits in `maxLines` lines of `w` pixels; at the
// last one, whatever doesn't fit is cut. width(text, size): its pixels.
template <class Width>
Fit Make(const std::string &text, std::initializer_list<int> sizes, int w, int maxLines, Width width) {
    Fit f;
    for (const int size : sizes) {
        f = {size, Wrap(text, size, w, width)};
        bool fits = int(f.lines.size()) <= maxLines;
        for (const std::string &l : f.lines) fits = fits && width(l, size) <= w;
        if (fits) return f;
    }
    if (int(f.lines.size()) > maxLines) {
        for (size_t i = maxLines; i < f.lines.size(); ++i) f.lines[maxLines - 1] += " " + f.lines[i];
        f.lines.resize(maxLines);
    }
    for (std::string &l : f.lines) l = Cut(l, f.size, w, width);
    return f;
}

}  // namespace labelfit
