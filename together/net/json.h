// A small JSON reader, for what tailscaled answers (tailnet.h). It reads a whole text into a
// tree, with a limit on depth and size, and refuses anything that isn't JSON. Header-only.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace json {

constexpr size_t kMostText = 4u << 20;   // bytes
constexpr int kMostDepth = 32;
constexpr size_t kMostValues = 100000;

struct Value {
    enum Type { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = kNull;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> items;                               // kArray
    std::vector<std::pair<std::string, Value>> members;     // kObject, in the text's order

    // A member, or a null value when this isn't an object or has no such member.
    const Value &operator[](const char *key) const {
        static const Value kNone;
        for (const auto &m : members)
            if (m.first == key) return m.second;
        return kNone;
    }
    // The text of a string, or "" for anything else.
    const std::string &text() const {
        static const std::string kNone;
        return type == kString ? string : kNone;
    }
};

namespace detail {

struct Reader {
    const std::string &s;
    size_t at = 0, values = 0;

    void Space() {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at;
    }
    bool Word(const char *w) {
        const size_t n = std::char_traits<char>::length(w);
        if (s.compare(at, n, w) != 0) return false;
        return at += n, true;
    }
    bool Hex4(unsigned &out) {
        if (s.size() - at < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[at++];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            out = out * 16 + unsigned(d);
        }
        return true;
    }
    static void Utf8(std::string &out, unsigned c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xc0 | c >> 6), out += char(0x80 | (c & 0x3f));
        else if (c < 0x10000) out += char(0xe0 | c >> 12), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
        else out += char(0xf0 | c >> 18), out += char(0x80 | ((c >> 12) & 0x3f)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
    }
    bool String(std::string &out) {
        if (at >= s.size() || s[at] != '"') return false;
        for (++at; at < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[at++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out += char(c);
                continue;
            }
            if (at >= s.size()) return false;
            switch (s[at++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned c1 = 0, c2 = 0;
                if (!Hex4(c1)) return false;
                if (c1 >= 0xd800 && c1 < 0xdc00) {   // the first half of a pair
                    if (!Word("\\u") || !Hex4(c2) || c2 < 0xdc00 || c2 >= 0xe000) return false;
                    c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                } else if (c1 >= 0xdc00 && c1 < 0xe000) {
                    return false;
                }
                Utf8(out, c1);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Number(double &out) {
        const size_t from = at;
        if (at < s.size() && s[at] == '-') ++at;
        const size_t digits = at;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
        if (at == digits || (at - digits > 1 && s[digits] == '0')) return false;
        if (at < s.size() && s[at] == '.') {
            const size_t frac = ++at;
            while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
            if (at == frac) return false;
        }
        if (at < s.size() && (s[at] == 'e' || s[at] == 'E')) {
            ++at;
            if (at < s.size() && (s[at] == '+' || s[at] == '-')) ++at;
            const size_t exp = at;
            while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
            if (at == exp) return false;
        }
        out = std::strtod(s.substr(from, at - from).c_str(), nullptr);
        return true;
    }
    bool Read(Value &v, int depth) {
        if (depth > kMostDepth || ++values > kMostValues) return false;
        Space();
        if (at >= s.size()) return false;
        const char c = s[at];
        if (c == '{') {
            v.type = Value::kObject;
            ++at, Space();
            if (at < s.size() && s[at] == '}') return ++at, true;
            for (;;) {
                Space();
                v.members.emplace_back();
                if (!String(v.members.back().first)) return false;
                Space();
                if (at >= s.size() || s[at++] != ':' || !Read(v.members.back().second, depth + 1)) return false;
                Space();
                if (at < s.size() && s[at] == ',') {
                    ++at;
                    continue;
                }
                return at < s.size() && s[at++] == '}';
            }
        }
        if (c == '[') {
            v.type = Value::kArray;
            ++at, Space();
            if (at < s.size() && s[at] == ']') return ++at, true;
            for (;;) {
                v.items.emplace_back();
                if (!Read(v.items.back(), depth + 1)) return false;
                Space();
                if (at < s.size() && s[at] == ',') {
                    ++at;
                    continue;
                }
                return at < s.size() && s[at++] == ']';
            }
        }
        if (c == '"') return v.type = Value::kString, String(v.string);
        if (Word("true")) return v.type = Value::kBool, v.boolean = true, true;
        if (Word("false")) return v.type = Value::kBool, true;
        if (Word("null")) return true;
        return v.type = Value::kNumber, Number(v.number);
    }
};

}  // namespace detail

// The whole of `text` as one JSON value. False for anything else, or for one too deep or large.
inline bool Parse(const std::string &text, Value &out) {
    out = Value{};
    if (text.size() > kMostText) return false;
    detail::Reader r{text};
    if (!r.Read(out, 0)) return false;
    r.Space();
    return r.at == text.size();
}

}  // namespace json
