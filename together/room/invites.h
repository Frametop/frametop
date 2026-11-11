// A room's invites, on its owner's side (docs/together.md, "Joining"): one-time codes, each good
// for ten minutes, and a limit on wrong guesses from one address. A code only proves its bearer
// was invited: who they are comes from Tailscale, and the owner still accepts them by hand.
//
// The rules only: the clock and the dice are handed in.
#pragma once

#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace room {

class Invites {
public:
    static constexpr double kGoodForS = 600;   // a code, from when it was made
    static constexpr size_t kMostCodes = 8;    // waiting at once: the oldest goes
    static constexpr int kMostWrong = 5;       // guesses from one address...
    static constexpr double kWrongForS = 600;  // ...in this long, before it is refused unheard
    static constexpr size_t kCodeLength = 8;

    // A new code: eight characters that read aloud without mix-ups (no 0, O, 1, I or L).
    std::string Make(double now, const std::function<uint64_t()> &dice) {
        static const char kLetters[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";   // 31
        Forget(now);
        std::string code;
        while (code.size() < kCodeLength) {
            const uint64_t roll = dice() & 31;   // 5 bits at a time: the 32nd value is rolled again
            if (roll < 31) code += kLetters[roll];
        }
        if (codes_.size() >= kMostCodes) codes_.erase(codes_.begin());
        codes_.push_back({code, now + kGoodForS});
        return code;
    }

    enum class Answer { kGood, kWrong, kRefused };

    // Someone at `address` shows `code`. A good one is used up. kRefused: too many wrong ones
    // came from there, and this one wasn't looked at.
    Answer Use(const std::string &code, const std::string &address, double now) {
        Forget(now);
        Wrong &w = wrong_[address];
        if (w.count >= kMostWrong) return Answer::kRefused;
        for (size_t i = 0; i < codes_.size(); ++i) {
            if (!Same(codes_[i].code, code)) continue;
            codes_.erase(codes_.begin() + long(i));
            if (!w.count) wrong_.erase(address);
            return Answer::kGood;
        }
        if (!w.count++) w.until = now + kWrongForS;
        return Answer::kWrong;
    }

    void Clear() { codes_.clear(), wrong_.clear(); }
    size_t waiting() const { return codes_.size(); }

private:
    struct Code {
        std::string code;
        double until;
    };
    struct Wrong {
        int count = 0;
        double until = 0;
    };

    // The same text, in the same time whatever differs. Case and a dash a person typed don't count.
    static bool Same(const std::string &ours, const std::string &theirs) {
        std::string typed;
        for (char c : theirs)
            if (c != '-' && c != ' ') typed += char(c >= 'a' && c <= 'z' ? c - 32 : c);
        unsigned diff = unsigned(ours.size() ^ typed.size());
        for (size_t i = 0; i < ours.size(); ++i) diff |= unsigned(uint8_t(ours[i]) ^ uint8_t(i < typed.size() ? typed[i] : 0));
        return diff == 0;
    }

    void Forget(double now) {
        for (size_t i = codes_.size(); i-- > 0;)
            if (codes_[i].until <= now) codes_.erase(codes_.begin() + long(i));
        for (auto it = wrong_.begin(); it != wrong_.end();) it = it->second.until <= now ? wrong_.erase(it) : std::next(it);
        // an address book of strangers doesn't grow without end
        while (wrong_.size() > 256) wrong_.erase(wrong_.begin());
    }

    std::vector<Code> codes_;
    std::map<std::string, Wrong> wrong_;
};

}  // namespace room
