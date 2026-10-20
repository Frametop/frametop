#pragma once
#include <functional>
#include <memory>
#include <string>
namespace framekeyboard {
// Optional Gamescope UTF-8 transport. No clipboard or application text is read.
// Discovery and commits progress only through nonblocking dispatch in ready().
class GamescopeText {
  public:
    using Clock = std::function<double()>;
    explicit GamescopeText(const std::string& socket, Clock clock = {});
    ~GamescopeText();
    bool ready();
    bool failed() const;
    bool commit(const std::string& text);
    bool pending() const;
    // Cancellation/timeout cannot undo an already submitted Wayland request.
    // Keep its ordering barrier until the sync arrives, even if the caller stops waiting.
    bool busy() const;
    void cancel();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace framekeyboard
