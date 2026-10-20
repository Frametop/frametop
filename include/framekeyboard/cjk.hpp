#pragma once

#include <memory>
#include <string>
#include <vector>

namespace framekeyboard {
// Local Pinyin/Hangul composition. The app owns delivery and only clears this
// state after a successful text commit. No engine learning API is used.
class CjkComposer {
  public:
    explicit CjkComposer(const std::string& method);
    ~CjkComposer();
    void type(char key);
    void backspace();
    void cancel();
    void cycle(int direction);
    void choose(int index);
    std::string preedit() const;
    std::string text() const;
    std::vector<std::string> candidates() const;
    int selected() const;
    bool empty() const;
    bool has_reading() const;
    bool chinese() const;
    static bool supported(const std::string& method);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace framekeyboard
