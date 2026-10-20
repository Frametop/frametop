#include "framekeyboard/config.hpp"
#include "bundled_profiles.hpp"
#include "framekeyboard/input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <json-c/json.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace framekeyboard {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
constexpr std::size_t max_file_bytes = 1024 * 1024;

Json decode(const std::string& text) {
    if (text.size() > max_file_bytes) {
        throw std::runtime_error("profile exceeds 1 MiB");
    }
    auto* parser = json_tokener_new_ex(32);
    if (!parser) {
        throw std::bad_alloc();
    }
    json_tokener_set_flags(parser, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value(json_tokener_parse_ex(parser, text.c_str(), static_cast<int>(text.size() + 1)),
               json_object_put);
    const auto error = json_tokener_get_error(parser);
    json_tokener_free(parser);
    if (error != json_tokener_success || !value || !json_object_is_type(value.get(), json_type_object)) {
        throw std::runtime_error("expected a valid JSON object");
    }
    return value;
}
json_object* field(json_object* object, const char* name, json_type type, bool optional = false) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value)) {
        if (optional) {
            return nullptr;
        }
        throw std::runtime_error(std::string("missing field: ") + name);
    }
    if (!json_object_is_type(value, type)) {
        throw std::runtime_error(std::string("wrong type: ") + name);
    }
    return value;
}
std::string text(json_object* object, const char* name, const std::string& fallback = "",
                 bool optional = false) {
    auto* value = field(object, name, json_type_string, optional);
    if (!value) {
        return fallback;
    }
    std::string result(json_object_get_string(value),
                       static_cast<std::size_t>(json_object_get_string_len(value)));
    if (result.size() > 256 ||
        std::any_of(result.begin(), result.end(), [](unsigned char c) { return c < 32; })) {
        throw std::runtime_error(std::string("invalid text: ") + name);
    }
    return result;
}
double number(json_object* object, const char* name, double low, double high, double fallback = -1) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value)) {
        if (fallback >= 0) {
            return fallback;
        }
        throw std::runtime_error(std::string("missing field: ") + name);
    }
    if (!json_object_is_type(value, json_type_double) && !json_object_is_type(value, json_type_int)) {
        throw std::runtime_error(std::string("expected number: ") + name);
    }
    const double result = json_object_get_double(value);
    if (!std::isfinite(result) || result < low || result > high) {
        throw std::runtime_error(std::string("out of range: ") + name);
    }
    return result;
}
std::string identifier(json_object* object, const char* name) {
    auto result = text(object, name);
    if (result.empty() || result.size() > 80 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_';
        })) {
        throw std::runtime_error(std::string("invalid identifier: ") + name);
    }
    return result;
}
void schema(json_object* object) {
    if (json_object_get_int(field(object, "schema_version", json_type_int)) != 1) {
        throw std::runtime_error("unsupported schema_version");
    }
}
std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open file");
    }
    std::string data(max_file_bytes + 1, '\0');
    file.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<std::size_t>(file.gcount()));
    if (data.size() > max_file_bytes) {
        throw std::runtime_error("file exceeds 1 MiB");
    }
    return data;
}
Color color(json_object* object, const char* name, const std::string& fallback = "") {
    const auto value = text(object, name, fallback, !fallback.empty());
    if (value.size() != 7 || value[0] != '#' ||
        !std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isxdigit(c); })) {
        throw std::runtime_error(std::string("expected #rrggbb: ") + name);
    }
    const auto packed = std::stoul(value.substr(1), nullptr, 16);
    return {static_cast<double>((packed >> 16) & 255) / 255,
            static_cast<double>((packed >> 8) & 255) / 255, static_cast<double>(packed & 255) / 255};
}
Selection selection(json_object* object, bool favorite = false) {
    const bool has_theme = field(object, "theme", json_type_string, favorite) != nullptr;
    return {identifier(object, "layout"), identifier(object, "language"),
            has_theme ? identifier(object, "theme") : "graphite"};
}

// Scan a directory as a transaction. Duplicate IDs never win by directory order.
template <class Profile, class Parser>
void scan(const fs::path& directory, const std::string& kind,
          std::map<std::string, Profile>& destination, Profiles& catalog, Parser parse) {
    if (directory.empty() || !fs::exists(directory)) {
        return;
    }
    std::map<std::string, std::pair<Profile, std::string>> candidates;
    std::set<std::string> duplicate;
    std::size_t count = 0;
    for (const auto& item : fs::directory_iterator(directory)) {
        if (item.path().extension() != ".json" || !item.is_regular_file()) {
            continue;
        }
        if (++count > 256) {
            throw std::runtime_error("too many profiles in " + directory.string());
        }
        try {
            auto profile = parse(read_file(item.path()));
            auto id = profile.id;
            if (candidates.contains(id)) {
                duplicate.insert(id);
                catalog.errors.push_back(item.path().filename().string() + ": duplicate profile ID " +
                                         id);
            } else {
                candidates.emplace(id, std::make_pair(std::move(profile), item.path().string()));
            }
        } catch (const std::exception& error) {
            catalog.errors.push_back(item.path().filename().string() + ": " + error.what());
        }
    }
    for (auto& [id, candidate] : candidates) {
        if (duplicate.contains(id)) {
            continue;
        }
        destination[id] = std::move(candidate.first);
        catalog.sources[kind + "/" + id] = candidate.second;
    }
}
} // namespace

Layout parse_layout(const std::string& json) {
    auto document = decode(json);
    auto* object = document.get();
    schema(object);
    Layout result;
    result.id = identifier(object, "id");
    result.name = text(object, "name");
    result.width = number(object, "width", 100, 4096);
    result.height = number(object, "height", 100, 2048);
    auto* keys = field(object, "keys", json_type_array);
    const auto count = json_object_array_length(keys);
    if (count == 0 || count > 256) {
        throw std::runtime_error("expected 1 to 256 keys");
    }
    std::set<std::string> ids;
    for (std::size_t i = 0; i < count; ++i) {
        auto* entry = json_object_array_get_idx(keys, i);
        Key key;
        key.id = identifier(entry, "id");
        key.label = text(entry, "label");
        key.secondary_label = text(entry, "secondary_label", "", true);
        const auto icon = text(entry, "icon", "", true);
        if (icon == "copy") {
            key.icon = Icon::Copy;
        } else if (icon == "paste") {
            key.icon = Icon::Paste;
        } else if (icon == "steam-frame") {
            key.icon = Icon::SteamFrame;
        } else if (icon == "steam-os") {
            key.icon = Icon::SteamOS;
        } else if (!icon.empty()) {
            throw std::runtime_error("unknown key icon");
        }
        key.bounds = {number(entry, "x", 0, result.width), number(entry, "y", 0, result.height),
                      number(entry, "width", 8, result.width),
                      number(entry, "height", 8, result.height)};
        auto* action = field(entry, "action", json_type_object);
        const auto kind = text(action, "kind");
        key.action = text(action, "value");
        if (kind == "key" && key_code(key.action) > 0) {
            key.action_kind = ActionKind::Key;
        } else if (kind == "shortcut" && (key.action == "copy" || key.action == "paste")) {
            key.action_kind = ActionKind::Shortcut;
        } else {
            throw std::runtime_error("unknown key action");
        }
        if (auto* sticky = field(entry, "sticky", json_type_boolean, true)) {
            key.sticky = json_object_get_boolean(sticky);
        }
        if (!ids.insert(key.id).second) {
            throw std::runtime_error("duplicate key ID");
        }
        const auto& r = key.bounds;
        if (r.x + r.width > result.width + .001 || r.y + r.height > result.height + .001) {
            throw std::runtime_error("key outside layout bounds");
        }
        for (const auto& other : result.keys) {
            const auto& b = other.bounds;
            if (std::min(r.x + r.width, b.x + b.width) - std::max(r.x, b.x) > .001 &&
                std::min(r.y + r.height, b.y + b.height) - std::max(r.y, b.y) > .001) {
                throw std::runtime_error("overlapping key bounds");
            }
        }
        result.keys.push_back(std::move(key));
    }
    return result;
}
Theme parse_theme(const std::string& json) {
    auto document = decode(json);
    auto* o = document.get();
    schema(o);
    Theme t;
    t.id = identifier(o, "id");
    t.name = text(o, "name");
    t.surface = color(o, "surface");
    t.top = color(o, "key_top");
    t.middle = color(o, "key_middle");
    t.bottom = color(o, "key_bottom");
    t.side_top = color(o, "key_side_top");
    t.side_bottom = color(o, "key_side_bottom");
    t.legend = color(o, "legend");
    t.hover = color(o, "hover", "#969ba3");
    t.latched = color(o, "latched", "#517ca6");
    t.disabled = color(o, "disabled", "#46494d");
    t.surface_radius = number(o, "surface_radius", 0, 40);
    t.radius = number(o, "key_radius", 0, 25);
    t.depth = number(o, "key_depth", 0, 15);
    t.travel = number(o, "key_press_travel", 0, t.depth);
    t.duration_ms = number(o, "press_duration_ms", 0, 500);
    t.font_size = number(o, "font_size", 8, 48, 20);
    t.small_font_size = number(o, "small_font_size", 8, 30, 13);
    t.padding = number(o, "padding", 4, 60, 20);
    return t;
}
Language parse_language(const std::string& json) {
    auto document = decode(json);
    auto* o = document.get();
    schema(o);
    Language l;
    l.id = identifier(o, "id");
    l.name = text(o, "name");
    l.locale = text(o, "locale");
    l.input_method = text(o, "input_method", "xkb", true);
    if (l.input_method != "xkb" && l.input_method != "japanese-romaji" &&
        l.input_method != "japanese-kana" && l.input_method != "korean-2set" &&
        l.input_method != "chinese-pinyin-simplified" &&
        l.input_method != "chinese-pinyin-traditional") {
        throw std::runtime_error("unsupported input method");
    }
    auto read_kana = [&](const char* field_name, auto& map) {
        if (auto* entries = field(o, field_name, json_type_object, true)) {
            json_object_object_foreach(entries, key, value) {
                (void)value;
                const auto kana = text(entries, key);
                if (kana.empty() || kana.size() > 12) {
                    throw std::runtime_error("invalid kana mapping");
                }
                map[key] = kana;
            }
        }
    };
    read_kana("kana", l.kana);
    read_kana("kana_shift", l.kana_shift);
    read_kana("composition_keys", l.composition_keys);
    read_kana("composition_shift", l.composition_shift);
    if (l.input_method == "japanese-kana" && l.kana.empty()) {
        throw std::runtime_error("direct Kana needs a kana mapping");
    }
    auto* map = field(o, "keymap", json_type_object);
    l.rules = text(map, "rules");
    l.model = text(map, "model");
    l.keymap = text(map, "layout");
    l.variant = text(map, "variant");
    // v1 has one active XKB group. Multi-group/IME profiles need a separate backend.
    if (l.keymap.empty() || l.keymap.find(',') != std::string::npos) {
        throw std::runtime_error("expected a single XKB layout");
    }
    auto* options = field(map, "options", json_type_array);
    if (json_object_array_length(options) != 0) {
        throw std::runtime_error("XKB options are not supported in v1");
    }
    auto* legends = field(o, "legends", json_type_object);
    if (text(legends, "source") != "keymap") {
        throw std::runtime_error("unsupported legend source");
    }
    auto* overrides = field(legends, "overrides", json_type_object);
    json_object_object_foreach(overrides, key, value) {
        (void)value;
        l.overrides[key] = text(overrides, key);
    }
    auto* fonts = field(o, "font_families", json_type_array);
    if (json_object_array_length(fonts) == 0 || json_object_array_length(fonts) > 8) {
        throw std::runtime_error("expected 1 to 8 font families");
    }
    for (std::size_t i = 0; i < json_object_array_length(fonts); ++i) {
        auto* font = json_object_array_get_idx(fonts, i);
        if (!json_object_is_type(font, json_type_string) || json_object_get_string_len(font) > 80) {
            throw std::runtime_error("invalid font family");
        }
        if (!l.font.empty()) {
            l.font += ',';
        }
        l.font += json_object_get_string(font);
    }
    if (auto* required = field(o, "required_keys", json_type_array, true)) {
        if (json_object_array_length(required) > 128) {
            throw std::runtime_error("too many required keys");
        }
        for (std::size_t i = 0; i < json_object_array_length(required); ++i) {
            auto* value = json_object_array_get_idx(required, i);
            if (!json_object_is_type(value, json_type_string)) {
                throw std::runtime_error("required key must be a string");
            }
            std::string name = json_object_get_string(value);
            if (!key_code(name)) {
                throw std::runtime_error("unknown required key");
            }
            l.required_keys.push_back(name);
        }
    }
    LanguageMap check(l);
    return l;
}
fs::path default_config_dir() {
    if (const char* base = std::getenv("XDG_CONFIG_HOME");
        base && *base && fs::path(base).is_absolute()) {
        return fs::path(base) / "framekeyboard";
    }
    if (const char* home = std::getenv("HOME")) {
        return fs::path(home) / ".config/framekeyboard";
    }
    throw std::runtime_error("HOME is unset; pass --config-dir");
}
Profiles load_profiles(const fs::path& data_dir, const fs::path& user_dir, const Profiles* previous) {
    Profiles p;
    for (const auto& resource : bundled::profiles) {
        const std::string json(resource.json);
        if (resource.kind == "layouts") {
            auto v = parse_layout(json);
            p.layouts[v.id] = std::move(v);
        }
        if (resource.kind == "languages") {
            auto v = parse_language(json);
            p.languages[v.id] = std::move(v);
        }
        if (resource.kind == "themes") {
            auto v = parse_theme(json);
            p.themes[v.id] = std::move(v);
        }
    }
    for (const auto& directory : {data_dir, user_dir}) {
        if (directory.empty()) {
            continue;
        }
        // Seed each source from its own last valid files, after lower-priority
        // sources. A malformed user override must not revert to a bundled file.
        if (previous) {
            for (const auto& [key, path] : previous->sources) {
                const auto slash = key.find('/');
                const auto kind = key.substr(0, slash), id = key.substr(slash + 1);
                if (!fs::exists(path) || fs::path(path).parent_path() != directory / kind) {
                    continue;
                }
                if (kind == "layouts") {
                    p.layouts[id] = previous->layouts.at(id);
                }
                if (kind == "languages") {
                    p.languages[id] = previous->languages.at(id);
                }
                if (kind == "themes") {
                    p.themes[id] = previous->themes.at(id);
                }
                p.sources[key] = path;
            }
        }
        try {
            scan(directory / "layouts", "layouts", p.layouts, p, parse_layout);
            scan(directory / "languages", "languages", p.languages, p, parse_language);
            scan(directory / "themes", "themes", p.themes, p, parse_theme);
        } catch (const std::exception& error) {
            p.errors.push_back(error.what());
        }
    }
    return p;
}
Settings load_settings(const fs::path& user_dir, std::vector<std::string>& errors) {
    Settings settings;
    const auto path = user_dir / "config.json";
    if (!fs::exists(path)) {
        return settings;
    }
    try {
        auto document = decode(read_file(path));
        schema(document.get());
        settings.active = selection(field(document.get(), "active", json_type_object));
        if (auto* pinned = field(document.get(), "pinned", json_type_boolean, true)) {
            settings.pinned = json_object_get_boolean(pinned);
        }
        if (auto* num = field(document.get(), "num_lock", json_type_boolean, true)) {
            settings.num_lock = json_object_get_boolean(num);
        }
        if (auto* hidden = field(document.get(), "hide_numpad", json_type_boolean, true)) {
            settings.hide_numpad = json_object_get_boolean(hidden);
        }
        if (auto* drag = field(document.get(), "use_drag_to_move", json_type_boolean, true)) {
            settings.use_drag_to_move = json_object_get_boolean(drag);
        }
        if (auto* leveling = field(document.get(), "auto_leveling", json_type_boolean, true)) {
            settings.auto_leveling = json_object_get_boolean(leveling);
        }
        if (auto* replace = field(document.get(), "replace_native_keyboard", json_type_boolean, true)) {
            settings.replace_native_keyboard = json_object_get_boolean(replace);
        }
        if (auto* startup = field(document.get(), "start_with_steamos", json_type_boolean, true)) {
            settings.start_with_steamos = json_object_get_boolean(startup);
        }
        if (auto* favorites = field(document.get(), "favorites", json_type_array, true)) {
            if (json_object_array_length(favorites) > 32) {
                throw std::runtime_error("too many favorites");
            }
            settings.favorites.clear();
            for (std::size_t i = 0; i < json_object_array_length(favorites); ++i) {
                auto* item = json_object_array_get_idx(favorites, i);
                settings.favorites.push_back(
                    {identifier(item, "id"), text(item, "name"), selection(item, true)});
            }
        }
    } catch (const std::exception& error) {
        errors.push_back("config.json: " + std::string(error.what()));
        settings = {};
    }
    return settings;
}
void validate_selection_profiles(const Profiles& profiles, const Selection& s) {
    if (!profiles.layouts.contains(s.layout) || !profiles.languages.contains(s.language) ||
        !profiles.themes.contains(s.theme)) {
        throw std::runtime_error("selected profile is unavailable");
    }
    const auto& layout = profiles.layouts.at(s.layout);
    const auto& language = profiles.languages.at(s.language);
    for (const auto& required : language.required_keys) {
        const bool found = std::any_of(layout.keys.begin(), layout.keys.end(), [&](const Key& key) {
            return key.action_kind == ActionKind::Key && key.action == required;
        });
        if (!found) {
            throw std::runtime_error("Language needs physical key " + required +
                                     ". Choose another layout.");
        }
    }
}
void validate_selection(const Profiles& profiles, const Selection& s) {
    validate_selection_profiles(profiles, s);
    LanguageMap check(profiles.languages.at(s.language));
}
namespace {
void write_settings(const fs::path& user_dir, const Selection& s, const Settings* settings) {
    fs::create_directories(user_dir);
    const auto path = user_dir / "config.json";
    // Preserve favorites and unknown settings. Never overwrite a malformed user file.
    Json document =
        fs::exists(path) ? decode(read_file(path)) : Json(json_object_new_object(), json_object_put);
    if (fs::exists(path)) {
        schema(document.get());
    }
    json_object_object_add(document.get(), "schema_version", json_object_new_int(1));
    auto* active = json_object_new_object();
    json_object_object_add(active, "layout", json_object_new_string(s.layout.c_str()));
    json_object_object_add(active, "language", json_object_new_string(s.language.c_str()));
    json_object_object_add(active, "theme", json_object_new_string(s.theme.c_str()));
    json_object_object_add(document.get(), "active", active);
    if (settings) {
        if (settings->favorites.size() > 32) {
            throw std::runtime_error("too many favorites");
        }
        auto* favorites = json_object_new_array();
        for (const auto& favorite : settings->favorites) {
            auto* item = json_object_new_object();
            json_object_object_add(item, "id", json_object_new_string(favorite.id.c_str()));
            json_object_object_add(item, "name", json_object_new_string(favorite.name.c_str()));
            json_object_object_add(item, "layout",
                                   json_object_new_string(favorite.selection.layout.c_str()));
            json_object_object_add(item, "language",
                                   json_object_new_string(favorite.selection.language.c_str()));
            json_object_object_add(item, "theme",
                                   json_object_new_string(favorite.selection.theme.c_str()));
            json_object_array_add(favorites, item);
        }
        json_object_object_add(document.get(), "favorites", favorites);
        json_object_object_add(document.get(), "pinned", json_object_new_boolean(settings->pinned));
        json_object_object_add(document.get(), "num_lock", json_object_new_boolean(settings->num_lock));
        json_object_object_add(document.get(), "hide_numpad",
                               json_object_new_boolean(settings->hide_numpad));
        json_object_object_add(document.get(), "use_drag_to_move",
                               json_object_new_boolean(settings->use_drag_to_move));
        json_object_object_add(document.get(), "auto_leveling",
                               json_object_new_boolean(settings->auto_leveling));
        json_object_object_add(document.get(), "replace_native_keyboard",
                               json_object_new_boolean(settings->replace_native_keyboard));
        json_object_object_add(document.get(), "start_with_steamos",
                               json_object_new_boolean(settings->start_with_steamos));
    }
    const std::string data =
        std::string(json_object_to_json_string_ext(document.get(), JSON_C_TO_STRING_PRETTY)) + '\n';
    std::string temporary = (user_dir / ".config-XXXXXX").string();
    const int fd = mkstemp(temporary.data());
    if (fd < 0) {
        throw std::runtime_error("cannot create settings temporary file");
    }
    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto written = write(fd, data.data() + offset, data.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::runtime_error("cannot write settings");
            }
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(fd) != 0) {
            throw std::runtime_error("cannot flush settings");
        }
        fs::rename(temporary, path);
        close(fd);
    } catch (...) {
        close(fd);
        unlink(temporary.c_str());
        throw;
    }
}
} // namespace
void save_selection(const fs::path& user_dir, const Selection& selection) {
    write_settings(user_dir, selection, nullptr);
}
void save_settings(const fs::path& user_dir, const Settings& settings) {
    write_settings(user_dir, settings.active, &settings);
}
} // namespace framekeyboard
