#include "framekeyboard/japanese.hpp"
#include <algorithm>
#include <dlfcn.h>
#include <glib.h>
#include <map>
#include <sstream>
#include <stdexcept>

namespace framekeyboard {
namespace {
std::string normalized(const std::string& text) {
    auto* value = g_utf8_normalize(text.c_str(), -1, G_NORMALIZE_DEFAULT_COMPOSE);
    std::string result = value ? value : text;
    g_free(value);
    return result;
}
void erase_character(std::string& text) {
    if (!text.empty()) {
        text.resize(static_cast<std::size_t>(
            g_utf8_find_prev_char(text.c_str(), text.c_str() + text.size()) - text.c_str()));
    }
}
const std::map<std::string, std::string>& roman_table() {
    static const auto table = [] {
        std::map<std::string, std::string> result;
        // Hepburn and common Japanese IME spellings, including small kana.
        std::istringstream entries(
            "a あ i い u う e え o お "
            "ka か ki き ku く ke け ko こ ca か ci し cu く ce せ co こ "
            "ga が gi ぎ gu ぐ ge げ go ご "
            "sa さ si し shi し su す se せ so そ za ざ zi じ ji じ zu ず ze ぜ zo ぞ "
            "ta た ti ち chi ち tu つ tsu つ te て to と da だ di ぢ du づ de で do ど "
            "na な ni に nu ぬ ne ね no の ha は hi ひ hu ふ fu ふ he へ ho ほ "
            "ba ば bi び bu ぶ be べ bo ぼ pa ぱ pi ぴ pu ぷ pe ぺ po ぽ "
            "ma ま mi み mu む me め mo も ya や yu ゆ yo よ ra ら ri り ru る re れ ro ろ "
            "wa わ wi うぃ wu う we うぇ wo を va ゔぁ vi ゔぃ vu ゔ ve ゔぇ vo ゔぉ "
            "sha しゃ shu しゅ sho しょ she しぇ cha ちゃ chu ちゅ cho ちょ che ちぇ "
            "ja じゃ ju じゅ jo じょ je じぇ fa ふぁ fi ふぃ fe ふぇ fo ふぉ "
            "fya ふゃ fyu ふゅ fyo ふょ tsa つぁ tsi つぃ tse つぇ tso つぉ "
            "tha てゃ thi てぃ thu てゅ the てぇ tho てょ dha でゃ dhi でぃ dhu でゅ dhe でぇ dho でょ "
            "twu とぅ dwu どぅ kwa くぁ gwa ぐぁ ye いぇ tcha っちゃ tchu っちゅ tcho っちょ "
            "xa ぁ xi ぃ xu ぅ xe ぇ xo ぉ xya ゃ xyu ゅ xyo ょ xtu っ xtsu っ xwa ゎ xka ヵ xke ヶ "
            "la ぁ li ぃ lu ぅ le ぇ lo ぉ lya ゃ lyu ゅ lyo ょ ltu っ ltsu っ lwa ゎ lka ヵ lke ヶ "
            "- ー , 、 . 。 [ 「 ] 」 / ・");
        std::string from, to;
        while (entries >> from >> to) {
            result[from] = to;
        }
        const std::map<std::string, std::string> palatal = {
            {"ky", "き"}, {"gy", "ぎ"}, {"sy", "し"}, {"zy", "じ"}, {"jy", "じ"},
            {"ty", "ち"}, {"cy", "ち"}, {"dy", "ぢ"}, {"ny", "に"}, {"hy", "ひ"},
            {"by", "び"}, {"py", "ぴ"}, {"my", "み"}, {"ry", "り"}};
        for (const auto& [prefix, kana] : palatal) {
            result[prefix + "a"] = kana + "ゃ";
            result[prefix + "u"] = kana + "ゅ";
            result[prefix + "o"] = kana + "ょ";
            result[prefix + "e"] = kana + "ぇ";
        }
        return result;
    }();
    return table;
}
} // namespace
// Load the existing system Anthy only on the first conversion. Its public C ABI
// keeps ordinary keyboard startup independent of the optional Japanese engine.
struct JapaneseComposer::Engine {
    struct Stat {
        int segments;
    };
    struct SegmentStat {
        int candidates, length;
    };
    struct Library {
        void* handle{};
        int (*init)(){};
        void (*quit)(){};
        void* (*create)(){};
        void (*release)(void*){};
        int (*encoding)(void*, int){};
        int (*set)(void*, const char*){};
        int (*stat)(void*, Stat*){};
        int (*segment_stat)(void*, int, SegmentStat*){};
        int (*get)(void*, int, int, char*, int){};
        template <class T> void symbol(T& fn, const char* name) {
            fn = reinterpret_cast<T>(dlsym(handle, name));
            if (!fn) {
                throw std::runtime_error("Installed Anthy has an incompatible API");
            }
        }
        Library() {
            handle = dlopen("libanthy.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!handle) {
                throw std::runtime_error("Japanese conversion needs the Anthy library");
            }
            try {
                symbol(init, "anthy_init");
                symbol(quit, "anthy_quit");
                symbol(create, "anthy_create_context");
                symbol(release, "anthy_release_context");
                symbol(encoding, "anthy_context_set_encoding");
                symbol(set, "anthy_set_string");
                symbol(stat, "anthy_get_stat");
                symbol(segment_stat, "anthy_get_segment_stat");
                symbol(get, "anthy_get_segment");
                if (init() != 0) {
                    throw std::runtime_error("Cannot initialize the Anthy dictionary");
                }
            } catch (...) {
                dlclose(handle);
                throw;
            }
        }
        ~Library() {
            quit();
            dlclose(handle);
        }
    };
    // Anthy initialization is process-wide. Share its lifetime across previews/tests.
    static Library& library() {
        static Library value;
        return value;
    }
    void* context{};
    Engine() {
        auto& api = library();
        context = api.create();
        if (!context) {
            throw std::runtime_error("Cannot create Japanese conversion context");
        }
        api.encoding(context, 2); // ANTHY_UTF8_ENCODING
    }
    ~Engine() { library().release(context); }
    std::vector<Segment> convert(const std::string& reading) {
        auto& api = library();
        Stat stat{};
        if (api.set(context, reading.c_str()) != 0 || api.stat(context, &stat) != 0 ||
            stat.segments <= 0 || stat.segments > 32) {
            throw std::runtime_error("Japanese conversion failed");
        }
        std::vector<Segment> result;
        for (int s = 0; s < stat.segments; ++s) {
            SegmentStat info{};
            if (api.segment_stat(context, s, &info) != 0 || info.candidates <= 0) {
                throw std::runtime_error("No conversion candidates");
            }
            Segment segment;
            for (int c = 0; c < std::min(info.candidates, 128); ++c) {
                char buffer[1024]{};
                if (api.get(context, s, c, buffer, sizeof(buffer)) < 0 ||
                    !g_utf8_validate(buffer, -1, nullptr)) {
                    throw std::runtime_error("Invalid Japanese conversion candidate");
                }
                segment.candidates.emplace_back(buffer);
            }
            result.push_back(std::move(segment));
        }
        return result;
    }
};
JapaneseComposer::JapaneseComposer() = default;
JapaneseComposer::~JapaneseComposer() = default;
void JapaneseComposer::append(const std::string& text) {
    reading_ = normalized(reading_ + text);
}
void JapaneseComposer::drain(bool finish) {
    const auto& table = roman_table();
    while (!pending_.empty()) {
        if (pending_[0] == 'n') {
            if (pending_.size() == 1) {
                if (finish) {
                    append("ん");
                    pending_.clear();
                }
                return;
            }
            const char next = pending_[1];
            if (next == '\'') {
                append("ん");
                pending_.erase(0, 2);
                continue;
            }
            if (next == 'n') {
                // nn commits one ん; retain the second n for nna/nni etc.
                if (pending_.size() == 2) {
                    if (finish) {
                        append("ん");
                        pending_.clear();
                    }
                    return;
                }
                append("ん");
                pending_.erase(0, std::string("aiueoy").find(pending_[2]) != std::string::npos ? 1 : 2);
                continue;
            }
            if (std::string("aiueoy").find(next) == std::string::npos) {
                append("ん");
                pending_.erase(0, 1);
                continue;
            }
        }
        if (pending_.size() > 1 && pending_[0] == pending_[1] &&
            std::string("bcdfghjklmpqrstvwxyz").find(pending_[0]) != std::string::npos) {
            append("っ");
            pending_.erase(0, 1);
            continue;
        }
        if (auto it = table.find(pending_); it != table.end()) {
            append(it->second);
            pending_.clear();
            return;
        }
        const bool prefix = std::any_of(table.begin(), table.end(), [&](const auto& pair) {
            return pair.first.starts_with(pending_);
        });
        if (prefix && !finish) {
            return;
        }
        append(pending_.substr(0, 1));
        pending_.erase(0, 1);
    }
}
void JapaneseComposer::roman(char key) {
    if (g_utf8_strlen(reading_.c_str(), -1) >= 30) {
        throw std::runtime_error("Commit this phrase before typing more");
    }
    unconvert();
    pending_ += static_cast<char>(g_ascii_tolower(key));
    drain(false);
}
void JapaneseComposer::kana(const std::string& text) {
    if (g_utf8_strlen(reading_.c_str(), -1) >= 30) {
        throw std::runtime_error("Commit this phrase before typing more");
    }
    unconvert();
    drain(true);
    append(text);
}
void JapaneseComposer::backspace() {
    if (converting()) {
        unconvert();
        return;
    }
    if (!pending_.empty()) {
        pending_.pop_back();
    } else {
        erase_character(reading_);
    }
}
void JapaneseComposer::convert() {
    if (converting()) {
        cycle(1);
        return;
    }
    drain(true);
    if (empty()) {
        return;
    }
    if (!engine_) {
        engine_ = std::make_unique<Engine>();
    }
    segments_ = engine_->convert(reading_);
    active_ = 0;
}
void JapaneseComposer::cycle(int direction) {
    if (!converting()) {
        return;
    }
    auto& s = segments_.at(static_cast<std::size_t>(active_));
    const int count = static_cast<int>(s.candidates.size());
    s.selected = (s.selected + direction + count) % count;
}
void JapaneseComposer::segment(int direction) {
    if (converting()) {
        active_ = std::clamp(active_ + direction, 0, segment_count() - 1);
    }
}
void JapaneseComposer::choose(int index) {
    if (converting() && index >= 0 &&
        index < static_cast<int>(segments_[static_cast<std::size_t>(active_)].candidates.size())) {
        segments_[static_cast<std::size_t>(active_)].selected = index;
    }
}
void JapaneseComposer::script(bool katakana) {
    drain(true);
    unconvert();
    std::string result;
    for (const char* p = reading_.c_str(); *p; p = g_utf8_next_char(p)) {
        auto c = g_utf8_get_char(p);
        if (katakana && c >= 0x3041 && c <= 0x3096) {
            c += 0x60;
        }
        if (!katakana && c >= 0x30a1 && c <= 0x30f6) {
            c -= 0x60;
        }
        char buffer[8]{};
        const auto n = g_unichar_to_utf8(c, buffer);
        result.append(buffer, static_cast<std::size_t>(n));
    }
    reading_ = result;
}
void JapaneseComposer::unconvert() {
    segments_.clear();
    active_ = 0;
}
void JapaneseComposer::cancel() {
    unconvert();
    reading_.clear();
    pending_.clear();
}
std::string JapaneseComposer::preedit(bool mark_active) const {
    if (!converting()) {
        return reading_ + (pending_ == "nn" ? "ん" : pending_);
    }
    std::string result;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const auto& segment = segments_[i];
        const bool selected = mark_active && static_cast<int>(i) == active_;
        if (selected) {
            result += "【";
        }
        result += segment.candidates[static_cast<std::size_t>(segment.selected)];
        if (selected) {
            result += "】";
        }
    }
    return result;
}
std::string JapaneseComposer::commit_text() {
    drain(true);
    return preedit();
}
std::vector<std::string> JapaneseComposer::candidates() const {
    return converting() ? segments_[static_cast<std::size_t>(active_)].candidates
                        : std::vector<std::string>{};
}
int JapaneseComposer::selected() const {
    return converting() ? segments_[static_cast<std::size_t>(active_)].selected : 0;
}
} // namespace framekeyboard
