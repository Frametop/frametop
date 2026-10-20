#pragma once
#include <memory>
#include <string>
#include <vector>

namespace framekeyboard {
// Pure local preedit state. Nothing reaches an application until explicit commit.
class JapaneseComposer {
  public:
    JapaneseComposer();
    ~JapaneseComposer();
    void roman(char key);
    void kana(const std::string& text);
    void backspace();
    void convert();
    void cycle(int direction);
    void segment(int direction);
    void choose(int index);
    void script(bool katakana);
    void cancel();
    void unconvert();
    std::string preedit(bool mark_active = false) const;
    std::string commit_text();
    bool empty() const { return reading_.empty() && pending_.empty(); }
    bool converting() const { return !segments_.empty(); }
    std::vector<std::string> candidates() const;
    int selected() const;
    int active_segment() const { return active_; }
    int segment_count() const { return static_cast<int>(segments_.size()); }

  private:
    struct Engine;
    std::unique_ptr<Engine> engine_;
    struct Segment {
        std::vector<std::string> candidates;
        int selected{};
    };
    std::vector<Segment> segments_;
    std::string reading_, pending_;
    int active_{};
    void drain(bool finish);
    void append(const std::string& text);
};
} // namespace framekeyboard
