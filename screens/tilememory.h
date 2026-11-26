// Each app's or display's last dashboard state (dashtiles.h's Board::Remember text), for the next
// time it opens: ft-screens' ~/.config/frametop-tiles.txt. Header only (screens/tests/dashtiles-test.cpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "tilestate.h"

namespace dashtiles {

// Each app's or display's last state (Board::Remember), by a key of the host's ("screen:2",
// "app:org.kde.dolphin", "remote:ID"): ft-screens' ~/.config/frametop-tiles.txt, a line each,
// "KEY<TAB>STATE".
class Memory {
public:
    static constexpr size_t kMax = 500;   // entries at most (the one used longest ago goes)

    // Spaces and control characters out (a key is one word), at most 200 bytes.
    static std::string Clean(const std::string &key) {
        std::string k;
        for (unsigned char c : key) k += c <= ' ' || c == 0x7f ? '_' : char(c);
        return k.substr(0, 200);
    }
    std::string Get(const std::string &key) const {
        const auto it = m_.find(Clean(key));
        return it == m_.end() ? std::string() : it->second.state;
    }
    // Its app or display is here again: the last to go when the memory is full.
    // False if it isn't remembered.
    bool Used(const std::string &key) {
        const auto it = m_.find(Clean(key));
        return it != m_.end() && (it->second.seq = ++seq_);
    }
    // True when it changed (the same again only counts as a use).
    bool Set(const std::string &key, const std::string &state) {
        const std::string k = Clean(key);
        if (k.empty() || state.find_first_of("\t\n") != std::string::npos) return false;
        auto it = m_.find(k);
        if (it != m_.end() && it->second.state == state) return it->second.seq = ++seq_, false;
        m_[k] = {state, ++seq_};
        while (m_.size() > kMax) {
            auto old = m_.begin();
            for (auto e = m_.begin(); e != m_.end(); ++e)
                if (e->second.seq < old->second.seq) old = e;
            m_.erase(old);
        }
        return true;
    }
    // Used longest ago first (Load keeps that order).
    std::string Text() const {
        std::vector<std::pair<uint64_t, const std::string *>> order;
        for (const auto &[k, e] : m_) order.push_back({e.seq, &k});
        std::sort(order.begin(), order.end());
        std::string s;
        for (const auto &[seq, k] : order) s += *k + "\t" + m_.at(*k).state + "\n";
        return s;
    }
    void Load(const std::string &text) {
        m_.clear();
        seq_ = 0;
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(at, end - at);
            at = end + 1;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos || tab == 0) continue;
            Set(line.substr(0, tab), line.substr(tab + 1));
        }
    }
    size_t Size() const { return m_.size(); }
    // `key` the one remembered as the centre display: every other tile's "centre" dropped, but not
    // `profileKey`'s (the profile in use saved it: it takes the centre when it comes back, Board::Restore).
    // A display chosen while the one before was gone left both saying it, so that one took the centre
    // again when it came back (Slay the Spire from dody, user, 2026-11-26). Their use order is kept.
    // True when one changed.
    bool OnlyCentre(const std::string &key, const std::string &profileKey = "") {
        const std::string k = Clean(key), p = profileKey.empty() ? "" : Clean(profileKey);
        bool changed = false;
        for (auto &[other, e] : m_) {
            State s;
            if (other == k || other == p || !dashtiles::Parse(e.state, &s) || !s.centre) continue;
            s.centre = false;
            e.state = dashtiles::Text(s), changed = true;
        }
        return changed;
    }

private:
    struct Entry {
        std::string state;
        uint64_t seq = 0;   // when it was set or used last (the one used longest ago goes first)
    };
    std::map<std::string, Entry> m_;
    uint64_t seq_ = 0;
};

// When the memory goes to its file: kDelayMs after the first change not yet written, so a burst
// (a profile's `tiles set`, a dozen windows mapping at once) is one write, off the frame it came in.
// Times are any steady milliseconds.
class SaveTimer {
public:
    static constexpr int64_t kDelayMs = 2000;

    void Changed(int64_t nowMs) {
        if (!dirty_) dirty_ = true, dueMs_ = nowMs + kDelayMs;
    }
    // True once it's due (it's written then: not due again until the next change).
    bool Due(int64_t nowMs) {
        if (!dirty_ || nowMs < dueMs_) return false;
        return dirty_ = false, true;
    }
    // True when there's a change not written (written now: shutting down).
    bool Flush() { return dirty_ ? (dirty_ = false, true) : false; }

private:
    bool dirty_ = false;
    int64_t dueMs_ = 0;
};

}  // namespace dashtiles
