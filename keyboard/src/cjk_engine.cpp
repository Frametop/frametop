#include "cjk_engine.hpp"
#include <memory>

#include <algorithm>
#include <glib.h>
#include <stdexcept>
#ifdef FRAMEKEYBOARD_HAVE_PYZY
#include <PyZy/InputContext.h>
#include <PyZy/Variant.h>
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
#include <hangul.h>
#endif

namespace framekeyboard {
class Engine final : public CjkEngine {
  public:
    explicit Engine(const std::string& method);
    ~Engine() override;
    void type(char key) override;
    void backspace() override;
    void cancel() override;
    void cycle(int direction) override;
    void choose(int index) override;
    std::string preedit() const override;
    std::string text() const override;
    std::vector<std::string> candidates() const override;
    int selected() const override;
    bool empty() const override;
    bool has_reading() const override;
    bool chinese() const override;
    static bool supported(const std::string& method);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

namespace {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
struct PinyinObserver : PyZy::InputContext::Observer {
    void commitText(PyZy::InputContext*, const std::string&) override {}
    void inputTextChanged(PyZy::InputContext*) override {}
    void cursorChanged(PyZy::InputContext*) override {}
    void preeditTextChanged(PyZy::InputContext*) override {}
    void auxiliaryTextChanged(PyZy::InputContext*) override {}
    void candidatesChanged(PyZy::InputContext*) override {}
};
struct PinyinLibrary {
    PinyinLibrary() {
        // PyZy's public API has no privacy switch. A non-directory forces its
        // SQLite in-memory fallback and prevents user DB writes.
        // We also never call commit/selectCandidate, which invoke learning.
        PyZy::InputContext::init("/dev/null", "/dev/null");
    }
    ~PinyinLibrary() { PyZy::InputContext::finalize(); }
};
std::shared_ptr<PinyinLibrary> pinyin_library() {
    static std::weak_ptr<PinyinLibrary> cached;
    auto library = cached.lock();
    if (!library) {
        library = std::make_shared<PinyinLibrary>();
        cached = library;
    }
    return library;
}
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
std::string utf8(const ucschar* text) {
    auto* converted = g_ucs4_to_utf8(text, -1, nullptr, nullptr, nullptr);
    std::string result = converted ? converted : "";
    g_free(converted);
    return result;
}
#endif
} // namespace
struct Engine::State {
    bool pinyin{};
    std::string keys, prefix, korean_text;
    // Undo a selected Pinyin segment without guessing syllable boundaries.
    std::vector<std::pair<std::string, std::string>> history;
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    // Declaration order ensures all contexts die before the shared library state.
    std::shared_ptr<PinyinLibrary> library;
    PinyinObserver observer;
    std::unique_ptr<PyZy::InputContext> context;
    void rebuild_pinyin() {
        context->reset();
        for (char ch : keys) {
            context->insert(ch);
        }
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    std::unique_ptr<HangulInputContext, decltype(&hangul_ic_delete)> hangul{nullptr, hangul_ic_delete};
    void rebuild_hangul() {
        // Replaying a bounded reading makes Backspace undo one physical jamo,
        // including a final consonant that moved into the following syllable.
        hangul_ic_reset(hangul.get());
        korean_text.clear();
        for (char ch : keys) {
            hangul_ic_process(hangul.get(), ch);
            korean_text += utf8(hangul_ic_get_commit_string(hangul.get()));
        }
        korean_text += utf8(hangul_ic_get_preedit_string(hangul.get()));
    }
#endif
};
bool Engine::supported(const std::string& method) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (method == "chinese-pinyin-simplified" || method == "chinese-pinyin-traditional") {
        return true;
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    if (method == "korean-2set") {
        return true;
    }
#endif
    (void)method;
    return false;
}
Engine::Engine(const std::string& method) : state_(std::make_unique<State>()) {
    if (!supported(method)) {
        throw std::runtime_error("This build lacks the selected Chinese/Korean input engine");
    }
    auto& s = *state_;
    s.pinyin = method.starts_with("chinese-");
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (s.pinyin) {
        s.library = pinyin_library();
        s.context.reset(PyZy::InputContext::create(PyZy::InputContext::FULL_PINYIN, &s.observer));
        if (!s.context) {
            throw std::runtime_error("Pinyin engine unavailable");
        }
        s.context->setProperty(PyZy::InputContext::PROPERTY_SPECIAL_PHRASE,
                               PyZy::Variant::fromBool(false));
        s.context->setProperty(PyZy::InputContext::PROPERTY_MODE_SIMP,
                               PyZy::Variant::fromBool(method == "chinese-pinyin-simplified"));
        // Check the installed dictionary before accepting a profile. No output,
        // history learning, or app focus is involved in this capability check.
        s.context->insert('n');
        s.context->insert('i');
        PyZy::Candidate candidate;
        const bool ready = s.context->getCandidate(0, candidate);
        s.context->reset();
        if (!ready) {
            throw std::runtime_error("Pinyin dictionary unavailable");
        }
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    if (!s.pinyin) {
        s.hangul.reset(hangul_ic_new("2"));
        if (!s.hangul) {
            throw std::runtime_error("Korean two-set engine unavailable");
        }
    }
#endif
}
Engine::~Engine() = default;
bool Engine::chinese() const {
    return state_->pinyin;
}
bool Engine::has_reading() const {
    return !state_->keys.empty();
}
bool Engine::empty() const {
    return state_->keys.empty() && state_->prefix.empty();
}
void Engine::type(char key) {
    auto& s = *state_;
    if (s.keys.size() + static_cast<std::size_t>(g_utf8_strlen(s.prefix.c_str(), -1)) >= 30) {
        throw std::runtime_error("Commit this composition before typing more");
    }
    if (s.pinyin) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
        key = g_ascii_tolower(key);
        if (s.context->insert(key)) {
            s.keys = s.context->inputText();
        }
#endif
    } else {
        s.keys += key;
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
        s.rebuild_hangul();
#endif
    }
}
void Engine::backspace() {
    auto& s = *state_;
    if (s.pinyin) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
        if (!s.keys.empty()) {
            s.context->removeCharBefore();
            s.keys = s.context->inputText();
        } else if (!s.history.empty()) {
            auto previous = std::move(s.history.back());
            s.history.pop_back();
            s.prefix = std::move(previous.first);
            s.keys = std::move(previous.second);
            s.rebuild_pinyin();
        }
#endif
    } else if (!s.keys.empty()) {
        s.keys.pop_back();
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
        s.rebuild_hangul();
#endif
    }
}
void Engine::cancel() {
    auto& s = *state_;
    s.keys.clear();
    s.prefix.clear();
    s.history.clear();
    s.korean_text.clear();
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (s.context) {
        s.context->reset();
    }
#endif
#ifdef FRAMEKEYBOARD_HAVE_HANGUL
    if (s.hangul) {
        hangul_ic_reset(s.hangul.get());
    }
#endif
}
std::vector<std::string> Engine::candidates() const {
    std::vector<std::string> result;
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->context) {
        PyZy::Candidate candidate;
        // Bound candidate work and menu size for controller interaction.
        for (std::size_t i = 0; i < 50 && state_->context->getCandidate(i, candidate); ++i) {
            result.push_back(candidate.text);
        }
    }
#endif
    return result;
}
int Engine::selected() const {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->context) {
        return static_cast<int>(state_->context->focusedCandidate());
    }
#endif
    return 0;
}
void Engine::cycle(int direction) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->context) {
        const int count = static_cast<int>(candidates().size());
        if (count) {
            state_->context->focusCandidate(
                static_cast<std::size_t>((selected() + direction + count) % count));
        }
    }
#else
    (void)direction;
#endif
}
void Engine::choose(int index) {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    auto& s = *state_;
    if (!s.context || index < 0 || index >= static_cast<int>(candidates().size()) ||
        !s.context->focusCandidate(static_cast<std::size_t>(index))) {
        return;
    }
    // Read the chosen segment and remaining phonetic input instead of invoking
    // PyZy's select/commit methods, which would learn the user's text.
    const auto chosen = s.context->conversionText();
    const auto rest = s.context->restText();
    if (chosen.empty()) {
        return;
    }
    s.history.emplace_back(s.prefix, s.keys);
    s.prefix += chosen;
    s.keys = rest;
    s.rebuild_pinyin();
#else
    (void)index;
#endif
}
std::string Engine::preedit() const {
    return state_->pinyin ? state_->prefix + state_->keys : state_->korean_text;
}
std::string Engine::text() const {
#ifdef FRAMEKEYBOARD_HAVE_PYZY
    if (state_->context) {
        return state_->prefix + state_->context->conversionText() + state_->context->restText();
    }
#endif
    return state_->korean_text;
}
} // namespace framekeyboard

// This factory is the only exported entry point. The module and host ship in
// the same release and share this internal C++ interface, not PyZy's ABI.
extern "C" __attribute__((visibility("default"))) framekeyboard::CjkEngine*
framekeyboard_create_cjk_v1(const char* method) {
    return new framekeyboard::Engine(method);
}
