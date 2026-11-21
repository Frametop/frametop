// Apple High Performance Screen Sharing client (MIT), part of apple_hp.h.
// Keyboard and pointer input: RFB key and pointer events, Apple's 0x10 keycode packet, keyboard
// layouts and the evdev-to-Apple key mapper.
#pragma once

#include "apple_hp_base.h"

namespace ft_apple_hp {

// ── RFB Client Input Builders & Linux Evdev -> Apple Keysym Translator ───────

// Synthetic internal keysym tag for raw macOS virtual keycodes (CGKeyCode 0x00..0xff) that have
// no X11 keysym in ScreensharingAgent's KeyMapDictionary (such as kVK_Function / Globe = 0x3f),
// dispatched on the wire via Apple's 0x10 EncryptedEventMessage instead of RFB 0x04 KeyEvent.
inline constexpr uint32_t kAppleMacKeycodeTag = 0x02000000u;
inline constexpr uint16_t kMacVkFunction = 0x003fu;        // kVK_Function (macOS Globe / Fn)
inline constexpr uint16_t kAppleKeyboardTypeAnsi = 0x003du; // Apple Silicon / ANSI keyboardType 61
inline constexpr uint32_t kAppleKeysymGlobe = kAppleMacKeycodeTag | kMacVkFunction;

inline bool IsAppleMacKeycodeKeysym(uint32_t ks, uint16_t *out_vk = nullptr) {
    if ((ks & 0xffffff00u) != kAppleMacKeycodeTag) return false;
    if (out_vk) *out_vk = uint16_t(ks & 0xffu);
    return true;
}

inline std::vector<uint8_t> BuildKeyEvent(bool down, uint32_t keysym) {
    std::vector<uint8_t> out = {0x04, uint8_t(down ? 1 : 0), 0x00, 0x00};
    AppendBe32(out, keysym);
    return out;
}

// Apple RFB client message 0x10 (HandleEncryptedEventMessage, 18 bytes):
//   [0]      0x10  opcode
//   [1]      0x03  flags (bit 0 | bit 1 -> ProcessKey sets use_keycode = 1 across viewer versions)
//   [2..17]  AES-128-ECB(cbc_key16, plain16), decrypted by screensharingd's [viewer+0x5f8] cryptor:
//              plain[0]      = 0xff             (keyboard event marker)
//              plain[1]      = down ? 1 : 0     (key-down state -> PostKeyEvent down)
//              plain[2..5]   = be32(keysym)     (0 when posting a raw macOS CGKeyCode)
//              plain[6..9]   = 0x00000000
//              plain[10]     = 0x00             (standalone key event; no coalesced pointer event)
//              plain[11]     = 0x00
//              plain[12..13] = be16(kb_type)    (0x003d = Apple keyboard type 61)
//              plain[14..15] = be16(mac_keycode)(e.g. 0x003f = kVK_Function / Globe)
inline bool BuildAppleKeycodeEvent0x10(const uint8_t ecb_key16[16], bool down, uint16_t mac_keycode,
                                       std::vector<uint8_t> *out, uint32_t keysym = 0,
                                       uint16_t keyboard_type = kAppleKeyboardTypeAnsi) {
    if (!out) return false;
    uint8_t plain[16]{};
    plain[0] = 0xff;
    plain[1] = uint8_t(down ? 1 : 0);
    WriteBe32(&plain[2], keysym);
    plain[10] = 0x00;
    WriteBe16(&plain[12], keyboard_type);
    WriteBe16(&plain[14], mac_keycode);
    out->assign(18, 0);
    (*out)[0] = 0x10;
    (*out)[1] = 0x03;
    return Aes128EcbCrypt(true, ecb_key16, plain, 16, out->data() + 2);
}

inline bool ParseAppleKeycodeEvent0x10(const uint8_t ecb_key16[16], const uint8_t *msg, size_t len,
                                       bool *out_down, uint16_t *out_keycode,
                                       uint16_t *out_kb_type = nullptr, uint32_t *out_keysym = nullptr) {
    if (!msg || len != 18 || msg[0] != 0x10 || (msg[1] & 0x01) == 0) return false;
    uint8_t plain[16]{};
    if (!Aes128EcbCrypt(false, ecb_key16, msg + 2, 16, plain)) return false;
    if (plain[0] != 0xff || (plain[10] != 0x00 && plain[10] != 0xff)) return false;
    if (out_down) *out_down = (plain[1] != 0);
    if (out_keysym) *out_keysym = ReadBe32(&plain[2]);
    if (out_kb_type) *out_kb_type = ReadBe16(&plain[12]);
    if (out_keycode) *out_keycode = ReadBe16(&plain[14]);
    return true;
}

inline std::vector<uint8_t> BuildPointerEvent(uint8_t button_mask, uint16_t x, uint16_t y) {
    std::vector<uint8_t> out = {0x05, button_mask};
    AppendBe16(out, x);
    AppendBe16(out, y);
    return out;
}

inline uint16_t ScalePointerCoord(double stream_px, uint32_t stream_dim, uint16_t backing_dim) {
    if (backing_dim == 0) return 0;
    const long max_coord = std::min<long>(long(backing_dim) - 1, 32767);
    if (stream_dim == 0) {
        return uint16_t(std::clamp<long>(std::lround(stream_px), 0, max_coord));
    }
    const double clamped = std::clamp(stream_px, 0.0, double(stream_dim - 1));
    if (stream_dim == backing_dim) {
        return uint16_t(std::clamp<long>(std::lround(clamped), 0, max_coord));
    }
    const double denom = double(stream_dim > 1 ? stream_dim - 1 : 1);
    const double scaled = (clamped * double(backing_dim - 1)) / denom;
    return uint16_t(std::clamp<long>(std::lround(scaled), 0, max_coord));
}

// Because BuildViewerInfo() identifies us as an Apple viewer (viewer->appleViewerFlag = 1),
// screensharingd's HandlePointerEventMessage skips the generic-VNC 0x02<->0x04 button swap and
// passes button bits directly to ScreensharingAgent's PostMouseEventIntoSession in CGMouseButton
// order: bit 0 (0x01) = Left (0), bit 1 (0x02) = Right (1), bit 2 (0x04) = Middle/Center (2).
inline uint8_t MouseButtonBit(uint32_t linux_btn) {
    switch (linux_btn) {
        case 0x110: return 0x01;  // BTN_LEFT   -> kCGMouseButtonLeft
        case 0x111: return 0x02;  // BTN_RIGHT  -> kCGMouseButtonRight
        case 0x112: return 0x04;  // BTN_MIDDLE -> kCGMouseButtonCenter
        default: return 0x00;
    }
}

// ── Keyboard Layouts & Evdev-to-Apple Keysym Mapper ──────────────────────────

enum class KeyboardLayout {
    kQwerty,
    kDvorak,
    kDvorakQwertyCmd,
    kColemak,
};

inline const char *KeyboardLayoutName(KeyboardLayout layout) {
    switch (layout) {
        case KeyboardLayout::kQwerty:          return "qwerty";
        case KeyboardLayout::kDvorak:          return "dvorak";
        case KeyboardLayout::kDvorakQwertyCmd: return "dvorak-qwerty-cmd";
        case KeyboardLayout::kColemak:         return "colemak";
    }
    return "qwerty";
}

inline bool ParseKeyboardLayoutName(const std::string &raw, KeyboardLayout *out) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c >= 'A' && c <= 'Z') s.push_back(char(c - 'A' + 'a'));
        else if (c == '_') s.push_back('-');
        else if (c != ' ' && c != '\t') s.push_back(c);
    }
    KeyboardLayout parsed = KeyboardLayout::kQwerty;
    if (s == "qwerty" || s == "us" || s == "default") parsed = KeyboardLayout::kQwerty;
    else if (s == "dvorak") parsed = KeyboardLayout::kDvorak;
    else if (s == "dvorak-qwerty-cmd" || s == "dvorak-cmd") parsed = KeyboardLayout::kDvorakQwertyCmd;
    else if (s == "colemak") parsed = KeyboardLayout::kColemak;
    else return false;
    if (out) *out = parsed;
    return true;
}

// Parses a physical Linux evdev key name (e.g. "capslock", "leftctrl", "a".."z", "0".."9",
// "f1".."f20", "minus", "leftbrace", or "code:58"). When `allow_none` is true, "none"/"disabled"/"off"
// resolves to 0 so a remap target can suppress a key cleanly. Digit strings "0".."9" always name
// physical number-row keys (KEY_0..KEY_9), never raw evdev code 1 (KEY_ESC).
inline bool ParseEvdevKeyName(const std::string &raw, uint32_t *out_code, bool allow_none = false) {
    std::string low;
    low.reserve(raw.size());
    for (char c : raw) {
        if (c >= 'A' && c <= 'Z') low.push_back(char(c - 'A' + 'a'));
        else if (c != ' ' && c != '\t') low.push_back(c);
    }
    if (low.empty()) return false;

    auto parse_raw_prefix = [&](const char *prefix) -> int {
        const size_t plen = std::strlen(prefix);
        if (low.rfind(prefix, 0) != 0 || low.size() <= plen) return -1;
        int v = 0;
        for (size_t i = plen; i < low.size(); ++i) {
            if (low[i] < '0' || low[i] > '9') return -1;
            v = v * 10 + (low[i] - '0');
            if (v > 255) return -1;
        }
        return (v >= 1 && v <= 255 && v != 194) ? v : -1;
    };
    const int raw_num = std::max(parse_raw_prefix("code:"), parse_raw_prefix("evdev:"));
    if (raw_num > 0) {
        if (out_code) *out_code = uint32_t(raw_num);
        return true;
    }

    std::string s;
    if (low.size() > 1) {
        s.reserve(low.size());
        for (char c : low) {
            if (c != '-' && c != '_') s.push_back(c);
        }
    } else {
        s = low;
    }
    if (s.empty()) return false;

    if (allow_none && (s == "none" || s == "disabled" || s == "off")) {
        if (out_code) *out_code = 0;
        return true;
    }
    if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') {
        static constexpr uint8_t kAlphaEvdev[26] = {
            30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
            49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
        };
        if (out_code) *out_code = kAlphaEvdev[s[0] - 'a'];
        return true;
    }
    if (s.size() == 1 && s[0] >= '0' && s[0] <= '9') {
        if (out_code) *out_code = (s[0] == '0') ? 11u : uint32_t(2 + (s[0] - '1'));
        return true;
    }
    if (s.size() >= 2 && s.size() <= 3 && s[0] == 'f' && s[1] >= '1' && s[1] <= '9') {
        int fn = s[1] - '0';
        if (s.size() == 3) {
            if (s[2] < '0' || s[2] > '9') fn = 0;
            else fn = fn * 10 + (s[2] - '0');
        }
        if (fn >= 1 && fn <= 20) {
            uint32_t code = 0;
            if (fn <= 10) code = uint32_t(59 + (fn - 1));
            else if (fn == 11) code = 87;
            else if (fn == 12) code = 88;
            else code = uint32_t(183 + (fn - 13));
            if (out_code) *out_code = code;
            return true;
        }
    }

    struct NamedKey {
        const char *name;
        uint32_t code;
    };
    static constexpr NamedKey kNamed[] = {
        {"esc", 1}, {"escape", 1},
        {"minus", 12}, {"hyphen", 12},
        {"equal", 13}, {"equals", 13},
        {"backspace", 14}, {"bksp", 14},
        {"tab", 15},
        {"leftbrace", 26}, {"lbrace", 26}, {"leftbracket", 26}, {"lbracket", 26},
        {"rightbrace", 27}, {"rbrace", 27}, {"rightbracket", 27}, {"rbracket", 27},
        {"enter", 28}, {"return", 28},
        {"leftctrl", 29}, {"lctrl", 29}, {"ctrl", 29}, {"control", 29}, {"leftcontrol", 29},
        {"semicolon", 39},
        {"apostrophe", 40}, {"quote", 40},
        {"grave", 41}, {"backtick", 41}, {"tilde", 41},
        {"leftshift", 42}, {"lshift", 42}, {"shift", 42},
        {"backslash", 43},
        {"comma", 51},
        {"dot", 52}, {"period", 52},
        {"slash", 53},
        {"rightshift", 54}, {"rshift", 54},
        {"kpasterisk", 55}, {"kpmultiply", 55},
        {"leftalt", 56}, {"lalt", 56}, {"alt", 56}, {"opt", 56}, {"option", 56}, {"leftopt", 56}, {"leftoption", 56},
        {"space", 57}, {"spacebar", 57},
        {"capslock", 58}, {"caps", 58},
        {"numlock", 69},
        {"kp7", 71}, {"kp8", 72}, {"kp9", 73},
        {"kpminus", 74}, {"kpsubtract", 74},
        {"kp4", 75}, {"kp5", 76}, {"kp6", 77},
        {"kpplus", 78}, {"kpadd", 78},
        {"kp1", 79}, {"kp2", 80}, {"kp3", 81},
        {"kp0", 82}, {"kpdot", 83}, {"kpdecimal", 83},
        {"102nd", 86}, {"iso102nd", 86},
        {"kpenter", 96},
        {"rightctrl", 97}, {"rctrl", 97}, {"rightcontrol", 97},
        {"kpslash", 98}, {"kpdivide", 98},
        {"rightalt", 100}, {"ralt", 100}, {"altgr", 100}, {"ropt", 100}, {"roption", 100}, {"rightopt", 100}, {"rightoption", 100},
        {"home", 102}, {"up", 103}, {"pageup", 104}, {"pgup", 104},
        {"left", 105}, {"right", 106}, {"end", 107}, {"down", 108}, {"pagedown", 109}, {"pgdn", 109},
        {"insert", 110}, {"ins", 110},
        {"delete", 111}, {"del", 111}, {"forwarddelete", 111},
        {"kpequal", 117},
        {"leftmeta", 125}, {"lmeta", 125}, {"meta", 125}, {"super", 125}, {"leftsuper", 125}, {"lsuper", 125},
        {"cmd", 125}, {"command", 125}, {"leftcmd", 125}, {"leftcommand", 125}, {"lcmd", 125}, {"win", 125}, {"lwin", 125},
        {"rightmeta", 126}, {"rmeta", 126}, {"rightsuper", 126}, {"rsuper", 126},
        {"rcmd", 126}, {"rcommand", 126}, {"rightcmd", 126}, {"rightcommand", 126}, {"rwin", 126},
        {"globe", 464}, {"fn", 464}, {"function", 464},
    };
    for (const auto &entry : kNamed) {
        if (s == entry.name) {
            if (out_code) *out_code = entry.code;
            return true;
        }
    }
    return false;
}

struct KeyboardMappingConfig {
    KeyboardLayout layout = KeyboardLayout::kQwerty;
    std::map<uint32_t, uint32_t> keymap;

    bool operator==(const KeyboardMappingConfig &o) const {
        return layout == o.layout && keymap == o.keymap;
    }
    bool operator!=(const KeyboardMappingConfig &o) const { return !(*this == o); }
};

inline void ApplyKeyboardSettings(const std::string &kb_layout,
                                  const std::vector<std::pair<std::string, std::string>> &pairs,
                                  KeyboardMappingConfig *cfg) {
    if (!cfg) return;
    if (!kb_layout.empty()) {
        KeyboardLayout parsed = KeyboardLayout::kQwerty;
        if (ParseKeyboardLayoutName(kb_layout, &parsed)) cfg->layout = parsed;
    }
    for (const auto &kv : pairs) {
        uint32_t from = 0, to = 0;
        if (!ParseEvdevKeyName(kv.first, &from, false)) continue;
        if (!ParseEvdevKeyName(kv.second, &to, true)) continue;
        if (from == to) cfg->keymap.erase(from);
        else cfg->keymap[from] = to;
    }
}

// Translates Linux evdev keycodes (<linux/input-event-codes.h>) to Apple ScreensharingAgent
// X11 keysyms (verified against ScreensharingAgent KeyMapDictionary_ConvertKeysym):
//   - Super_L/R (0xffeb/0xffec) -> kVK_Command / kVK_RightCommand (0x37/0x36)
//   - Meta_L/R  (0xffe7/0xffe8) -> kVK_Option  / kVK_RightOption  (0x3a/0x3d)
//   - Printable ASCII keys carry their target character keysym ('!' vs '1', 'A' vs 'a',
//     and layout-translated characters for Dvorak / Colemak) because ScreensharingAgent builds
//     its reverse keysym-to-keycode dictionary from the Mac's active TIS keyboard layout.
//   - Applies optional physical-key remaps (`cfg_.keymap`, e.g. Caps Lock -> Left Control)
//     before Caps Lock toggle and modifier tracking, reference-counting shared target keysyms.
//   - Preserves Frametop's Meta holdback + KEY_F24 (194) combo suppression so Frametop hotkeys
//     (Meta+Shift+R, etc.) never leak Command taps to macOS.
class EvdevKeyMapper {
public:
    static constexpr uint32_t kEvKeyLeftShift = 42;
    static constexpr uint32_t kEvKeyRightShift = 54;
    static constexpr uint32_t kEvKeyCapsLock = 58;
    static constexpr uint32_t kEvKeyLeftMeta = 125;
    static constexpr uint32_t kEvKeyRightMeta = 126;
    static constexpr uint32_t kEvKeyF24 = 194;
    static constexpr uint32_t kEvKeyFn = 464;  // Linux KEY_FN (0x1d0) -> macOS Globe / Fn (kVK_Function = 0x3f)

    using EmitFn = std::function<void(bool down, uint32_t keysym)>;

    EvdevKeyMapper() = default;
    explicit EvdevKeyMapper(KeyboardMappingConfig cfg) : cfg_(std::move(cfg)) {}

    const KeyboardMappingConfig &config() const { return cfg_; }

    void SetConfig(const KeyboardMappingConfig &cfg, const EmitFn &emit) {
        if (cfg == cfg_) return;
        Blur(emit);
        if (cfg.keymap.find(kEvKeyCapsLock) != cfg.keymap.end()) caps_ = false;
        cfg_ = cfg;
    }

    static bool LookupLayoutAscii(KeyboardLayout layout, uint32_t evdev, char *unshifted, char *shifted) {
        if (evdev >= 2 && evdev <= 11) {
            static constexpr char kNum[11] = "1234567890";
            static constexpr char kNumS[11] = "!@#$%^&*()";
            *unshifted = kNum[evdev - 2];
            *shifted = kNumS[evdev - 2];
            return true;
        }
        if (evdev == 41) {
            *unshifted = '`';
            *shifted = '~';
            return true;
        }
        if (evdev == 43) {
            *unshifted = '\\';
            *shifted = '|';
            return true;
        }
        if (layout == KeyboardLayout::kDvorak) {
            if (evdev == 12) { *unshifted = '['; *shifted = '{'; return true; }
            if (evdev == 13) { *unshifted = ']'; *shifted = '}'; return true; }
            if (evdev >= 16 && evdev <= 25) {
                static constexpr char kRowQ[11] = "',.pyfgcrl";
                static constexpr char kRowQS[11] = "\"<>PYFGCRL";
                *unshifted = kRowQ[evdev - 16];
                *shifted = kRowQS[evdev - 16];
                return true;
            }
            if (evdev == 26) { *unshifted = '/'; *shifted = '?'; return true; }
            if (evdev == 27) { *unshifted = '='; *shifted = '+'; return true; }
            if (evdev >= 30 && evdev <= 39) {
                static constexpr char kRowA[11] = "aoeuidhtns";
                static constexpr char kRowAS[11] = "AOEUIDHTNS";
                *unshifted = kRowA[evdev - 30];
                *shifted = kRowAS[evdev - 30];
                return true;
            }
            if (evdev == 40) { *unshifted = '-'; *shifted = '_'; return true; }
            if (evdev == 44) { *unshifted = ';'; *shifted = ':'; return true; }
            if (evdev >= 45 && evdev <= 53) {
                static constexpr char kRowZ[10] = "qjkxbmwvz";
                static constexpr char kRowZS[10] = "QJKXBMWVZ";
                *unshifted = kRowZ[evdev - 45];
                *shifted = kRowZS[evdev - 45];
                return true;
            }
            return false;
        }
        if (layout == KeyboardLayout::kColemak) {
            if (evdev == 12) { *unshifted = '-'; *shifted = '_'; return true; }
            if (evdev == 13) { *unshifted = '='; *shifted = '+'; return true; }
            if (evdev >= 16 && evdev <= 24) {
                static constexpr char kRowQ[10] = "qwfpgjluy";
                static constexpr char kRowQS[10] = "QWFPGJLUY";
                *unshifted = kRowQ[evdev - 16];
                *shifted = kRowQS[evdev - 16];
                return true;
            }
            if (evdev == 25) { *unshifted = ';'; *shifted = ':'; return true; }
            if (evdev == 26) { *unshifted = '['; *shifted = '{'; return true; }
            if (evdev == 27) { *unshifted = ']'; *shifted = '}'; return true; }
            if (evdev >= 30 && evdev <= 39) {
                static constexpr char kRowA[11] = "arstdhneio";
                static constexpr char kRowAS[11] = "ARSTDHNEIO";
                *unshifted = kRowA[evdev - 30];
                *shifted = kRowAS[evdev - 30];
                return true;
            }
            if (evdev == 40) { *unshifted = '\''; *shifted = '"'; return true; }
            if (evdev >= 44 && evdev <= 50) {
                static constexpr char kRowZ[8] = "zxcvbkm";
                static constexpr char kRowZS[8] = "ZXCVBKM";
                *unshifted = kRowZ[evdev - 44];
                *shifted = kRowZS[evdev - 44];
                return true;
            }
            if (evdev == 51) { *unshifted = ','; *shifted = '<'; return true; }
            if (evdev == 52) { *unshifted = '.'; *shifted = '>'; return true; }
            if (evdev == 53) { *unshifted = '/'; *shifted = '?'; return true; }
            return false;
        }
        // Default ANSI QWERTY (also used for Command combos in kDvorakQwertyCmd).
        if (evdev == 12) { *unshifted = '-'; *shifted = '_'; return true; }
        if (evdev == 13) { *unshifted = '='; *shifted = '+'; return true; }
        if (evdev >= 16 && evdev <= 25) {
            static constexpr char kRowQ[11] = "qwertyuiop";
            static constexpr char kRowQS[11] = "QWERTYUIOP";
            *unshifted = kRowQ[evdev - 16];
            *shifted = kRowQS[evdev - 16];
            return true;
        }
        if (evdev == 26) { *unshifted = '['; *shifted = '{'; return true; }
        if (evdev == 27) { *unshifted = ']'; *shifted = '}'; return true; }
        if (evdev >= 30 && evdev <= 38) {
            static constexpr char kRowA[10] = "asdfghjkl";
            static constexpr char kRowAS[10] = "ASDFGHJKL";
            *unshifted = kRowA[evdev - 30];
            *shifted = kRowAS[evdev - 30];
            return true;
        }
        if (evdev == 39) { *unshifted = ';'; *shifted = ':'; return true; }
        if (evdev == 40) { *unshifted = '\''; *shifted = '"'; return true; }
        if (evdev >= 44 && evdev <= 50) {
            static constexpr char kRowZ[8] = "zxcvbnm";
            static constexpr char kRowZS[8] = "ZXCVBNM";
            *unshifted = kRowZ[evdev - 44];
            *shifted = kRowZS[evdev - 44];
            return true;
        }
        if (evdev == 51) { *unshifted = ','; *shifted = '<'; return true; }
        if (evdev == 52) { *unshifted = '.'; *shifted = '>'; return true; }
        if (evdev == 53) { *unshifted = '/'; *shifted = '?'; return true; }
        return false;
    }

    static uint32_t LookupKeysym(uint32_t evdev, bool shift, bool caps,
                                 KeyboardLayout layout = KeyboardLayout::kQwerty) {
        char unshifted = 0, shifted = 0;
        if (LookupLayoutAscii(layout, evdev, &unshifted, &shifted)) {
            const bool is_letter = (unshifted >= 'a' && unshifted <= 'z');
            const bool use_shifted = is_letter ? (shift != caps) : shift;
            return uint32_t(uint8_t(use_shifted ? shifted : unshifted));
        }
        switch (evdev) {
            case 1:   return 0xff1b;  // KEY_ESC -> Escape
            case 14:  return 0xff08;  // KEY_BACKSPACE -> BackSpace (kVK_Delete 0x33)
            case 15:  return 0xff09;  // KEY_TAB -> Tab
            case 28:  return 0xff0d;  // KEY_ENTER -> Return
            case 29:  return 0xffe3;  // KEY_LEFTCTRL -> Control_L (0x3b)
            case 42:  return 0xffe1;  // KEY_LEFTSHIFT -> Shift_L (0x38)
            case 54:  return 0xffe2;  // KEY_RIGHTSHIFT -> Shift_R (0x3c)
            case 55:  return 0xffaa;  // KEY_KPASTERISK -> KP_Multiply
            case 56:  return 0xffe7;  // KEY_LEFTALT -> Meta_L -> Mac Option_L (0x3a)
            case 57:  return 0x0020;  // KEY_SPACE
            case 58:  return 0xffe5;  // KEY_CAPSLOCK -> Caps_Lock (0x39)
            case 59: case 60: case 61: case 62: case 63:
            case 64: case 65: case 66: case 67: case 68:
                return 0xffbe + (evdev - 59);  // F1..F10
            case 69:  return 0xff7f;  // KEY_NUMLOCK -> Clear
            case 71:  return 0xffb7;  // KP_7
            case 72:  return 0xffb8;  // KP_8
            case 73:  return 0xffb9;  // KP_9
            case 74:  return 0xffad;  // KP_Subtract
            case 75:  return 0xffb4;  // KP_4
            case 76:  return 0xffb5;  // KP_5
            case 77:  return 0xffb6;  // KP_6
            case 78:  return 0xffab;  // KP_Add
            case 79:  return 0xffb1;  // KP_1
            case 80:  return 0xffb2;  // KP_2
            case 81:  return 0xffb3;  // KP_3
            case 82:  return 0xffb0;  // KP_0
            case 83:  return 0xffae;  // KP_Decimal
            case 86:  return shift ? '>' : '<';  // KEY_102ND
            case 87:  return 0xffc8;  // F11
            case 88:  return 0xffc9;  // F12
            case 96:  return 0xff8d;  // KP_Enter
            case 97:  return 0xffe4;  // KEY_RIGHTCTRL -> Control_R (0x3e)
            case 98:  return 0xffaf;  // KP_Divide
            case 100: return 0xffe8;  // KEY_RIGHTALT -> Meta_R -> Mac Option_R (0x3d)
            case 102: return 0xff50;  // KEY_HOME
            case 103: return 0xff52;  // KEY_UP
            case 104: return 0xff55;  // KEY_PAGEUP
            case 105: return 0xff51;  // KEY_LEFT
            case 106: return 0xff53;  // KEY_RIGHT
            case 107: return 0xff57;  // KEY_END
            case 108: return 0xff54;  // KEY_DOWN
            case 109: return 0xff56;  // KEY_PAGEDOWN
            case 110: return 0xff63;  // KEY_INSERT -> Insert/Help
            case 111: return 0xffff;  // KEY_DELETE -> Forward Delete (0x75)
            case 117: return 0xffbd;  // KEY_KPEQUAL -> KP_Equal
            case 125: return 0xffeb;  // KEY_LEFTMETA -> Super_L -> Mac Command_L (0x37)
            case 126: return 0xffec;  // KEY_RIGHTMETA -> Super_R -> Mac Command_R (0x36)
            case 183: case 184: case 185: case 186:
            case 187: case 188: case 189: case 190:
                return 0xffca + (evdev - 183);  // F13..F20
            case 464: return kAppleKeysymGlobe; // KEY_FN / "globe" -> kVK_Function (0x3f) via 0x10 packet
            default:  return 0;
        }
    }

    void Process(uint32_t raw_code, bool down, const EmitFn &emit) {
        if (raw_code == kEvKeyF24) {
            held_meta_raw_ = 0;  // Frametop ate a Meta combo; suppress held Command.
            held_meta_code_ = 0;
            return;
        }

        if (!down) {
            if (suppressed_raw_.erase(raw_code) > 0) return;
            if (held_meta_raw_ != 0 && raw_code == held_meta_raw_) {
                const uint32_t mcode = held_meta_code_;
                held_meta_raw_ = 0;
                held_meta_code_ = 0;
                const uint32_t mks = LookupKeysym(mcode, false, false, EffectiveLayout());
                if (mks) {
                    emit(true, mks);
                    emit(false, mks);
                }
                return;
            }
            const auto it = active_raw_.find(raw_code);
            if (it == active_raw_.end()) return;
            const ActiveKey entry = it->second;
            active_raw_.erase(it);
            if ((entry.mapped_code == kEvKeyLeftShift || entry.mapped_code == kEvKeyRightShift) && shift_refs_ > 0) {
                --shift_refs_;
            }
            if ((entry.mapped_code == kEvKeyLeftMeta || entry.mapped_code == kEvKeyRightMeta) && meta_refs_ > 0) {
                --meta_refs_;
            }
            const auto kit = keysym_refs_.find(entry.keysym);
            if (kit != keysym_refs_.end()) {
                if (--kit->second == 0) {
                    keysym_refs_.erase(kit);
                    emit(false, entry.keysym);
                }
            }
            return;
        }

        // Key-down: handle auto-repeat on an already-active or held-back physical key cleanly.
        if (held_meta_raw_ != 0 && raw_code == held_meta_raw_) return;
        const auto existing = active_raw_.find(raw_code);
        if (existing != active_raw_.end()) {
            emit(true, existing->second.keysym);
            return;
        }

        uint32_t code = raw_code;
        const auto map_it = cfg_.keymap.find(raw_code);
        if (map_it != cfg_.keymap.end()) {
            if (map_it->second == 0) {
                suppressed_raw_.insert(raw_code);
                return;
            }
            code = map_it->second;
        }

        const bool is_shift = (code == kEvKeyLeftShift || code == kEvKeyRightShift);
        const bool is_meta = (code == kEvKeyLeftMeta || code == kEvKeyRightMeta);
        if (code == kEvKeyCapsLock) caps_ = !caps_;

        if (is_meta && meta_refs_ == 0 && held_meta_raw_ == 0) {
            held_meta_raw_ = raw_code;
            held_meta_code_ = code;
            return;
        }
        if (held_meta_raw_ != 0) {
            const uint32_t mraw = held_meta_raw_;
            const uint32_t mcode = held_meta_code_;
            held_meta_raw_ = 0;
            held_meta_code_ = 0;
            const uint32_t mks = LookupKeysym(mcode, false, false, KeyboardLayout::kQwerty);
            if (mks) {
                active_raw_[mraw] = ActiveKey{mcode, mks};
                ++meta_refs_;
                if (++keysym_refs_[mks] == 1) emit(true, mks);
            }
        }

        if (is_shift) ++shift_refs_;
        if (is_meta) ++meta_refs_;
        const uint32_t ks = LookupKeysym(code, shift_refs_ > 0, caps_, EffectiveLayout());
        if (!ks) {
            if (is_shift && shift_refs_ > 0) --shift_refs_;
            if (is_meta && meta_refs_ > 0) --meta_refs_;
            return;
        }
        active_raw_[raw_code] = ActiveKey{code, ks};
        if (++keysym_refs_[ks] == 1) emit(true, ks);
    }

    void Blur(const EmitFn &emit) {
        held_meta_raw_ = 0;
        held_meta_code_ = 0;
        shift_refs_ = 0;
        meta_refs_ = 0;
        active_raw_.clear();
        suppressed_raw_.clear();
        for (const auto &kv : keysym_refs_) emit(false, kv.first);
        keysym_refs_.clear();
    }

private:
    struct ActiveKey {
        uint32_t mapped_code = 0;
        uint32_t keysym = 0;
    };

    KeyboardLayout EffectiveLayout() const {
        if (cfg_.layout == KeyboardLayout::kDvorakQwertyCmd) {
            return (meta_refs_ > 0 || held_meta_raw_ != 0) ? KeyboardLayout::kQwerty : KeyboardLayout::kDvorak;
        }
        return cfg_.layout;
    }

    KeyboardMappingConfig cfg_{};
    uint32_t held_meta_raw_ = 0;
    uint32_t held_meta_code_ = 0;
    uint32_t shift_refs_ = 0;
    uint32_t meta_refs_ = 0;
    bool caps_ = false;
    std::map<uint32_t, ActiveKey> active_raw_;
    std::map<uint32_t, uint32_t> keysym_refs_;
    std::set<uint32_t> suppressed_raw_;
};

}  // namespace ft_apple_hp
