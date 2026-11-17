// The Chrome DevTools Protocol's wire, as steamcdp.cpp speaks it to Steam's CEF: the WebSocket
// handshake and frames (RFC 6455, a client's side: text frames out, masked), the target list
// (GET /json: each page's title and socket), and the messages it sends (Input.dispatchMouseEvent,
// Runtime.evaluate). Only what that needs: no fragmented frames out, JSON read by scanning for keys.
//
// Pure, no sockets: screens/tests/cdpwire-test.cpp.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cdpwire {

// "ws://host:port/path" -> "host:port", "/path". False when it isn't one.
inline bool SplitWsUrl(const std::string &url, std::string *hostPort, std::string *path) {
    const std::string scheme = "ws://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    const size_t slash = url.find('/', scheme.size());
    if (slash == std::string::npos || slash == scheme.size()) return false;
    *hostPort = url.substr(scheme.size(), slash - scheme.size());
    *path = url.substr(slash);
    return true;
}

inline std::string UpgradeRequest(const std::string &hostPort, const std::string &path, const std::string &key) {
    return "GET " + path + " HTTP/1.1\r\nHost: " + hostPort +
           "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
           "\r\nSec-WebSocket-Version: 13\r\n\r\n";
}
// The server's answer to it, up to its blank line: whether it switched.
inline bool Upgraded(const std::string &head) {
    const size_t eol = head.find("\r\n");
    return head.compare(0, 9, "HTTP/1.1 ") == 0 && head.compare(9, 3, "101") == 0 && eol != std::string::npos;
}

// A client's text frame: masked with `mask`, as RFC 6455 says a client's must be.
inline std::string TextFrame(const std::string &text, const uint8_t mask[4]) {
    std::string f;
    f += char(0x81);
    const uint64_t n = text.size();
    if (n < 126) {
        f += char(0x80 | n);
    } else if (n < 65536) {
        f += char(0x80 | 126), f += char(n >> 8), f += char(n & 0xff);
    } else {
        f += char(0x80 | 127);
        for (int i = 7; i >= 0; --i) f += char((n >> (8 * i)) & 0xff);
    }
    f.append(reinterpret_cast<const char *>(mask), 4);
    for (size_t i = 0; i < n; ++i) f += char(uint8_t(text[i]) ^ mask[i % 4]);
    return f;
}

// One whole frame off the front of `buf` (a server's: unmasked, though a masked one is read too):
// its opcode (1 text, 8 close, 9 ping, ...) and payload. False while `buf` holds less than one.
inline bool TakeFrame(std::string &buf, int *op, std::string *payload) {
    if (buf.size() < 2) return false;
    const uint8_t b0 = uint8_t(buf[0]), b1 = uint8_t(buf[1]);
    size_t at = 2;
    uint64_t n = b1 & 0x7f;
    if (n == 126) {
        if (buf.size() < 4) return false;
        n = uint64_t(uint8_t(buf[2])) << 8 | uint8_t(buf[3]);
        at = 4;
    } else if (n == 127) {
        if (buf.size() < 10) return false;
        n = 0;
        for (int i = 0; i < 8; ++i) n = n << 8 | uint8_t(buf[2 + i]);
        at = 10;
    }
    const bool masked = b1 & 0x80;
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (buf.size() < at + 4) return false;
        for (int i = 0; i < 4; ++i) mask[i] = uint8_t(buf[at + i]);
        at += 4;
    }
    if (buf.size() - at < n) return false;
    *op = b0 & 0x0f;
    payload->assign(buf, at, size_t(n));
    if (masked)
        for (size_t i = 0; i < payload->size(); ++i) (*payload)[i] = char(uint8_t((*payload)[i]) ^ mask[i % 4]);
    buf.erase(0, at + size_t(n));
    return true;
}

// A JSON string literal of `s`.
inline std::string Quote(const std::string &s) {
    std::string q = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') q += '\\', q += c;
        else if (uint8_t(c) < 0x20) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", unsigned(uint8_t(c)));
            q += u;
        } else q += c;
    }
    return q + "\"";
}

// The JSON string starting at json[at] (its opening quote), unescaped; *end just past it. False:
// not a string, or cut off. \uXXXX past ASCII comes out as UTF-8.
inline bool ReadString(const std::string &json, size_t at, std::string *out, size_t *end) {
    if (at >= json.size() || json[at] != '"') return false;
    out->clear();
    for (size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') return *end = i + 1, true;
        if (c != '\\') {
            *out += c;
            continue;
        }
        if (++i >= json.size()) return false;
        switch (json[i]) {
            case 'n': *out += '\n'; break;
            case 't': *out += '\t'; break;
            case 'r': *out += '\r'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'u': {
                if (i + 4 >= json.size()) return false;
                const unsigned cp = unsigned(std::strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
                i += 4;
                if (cp < 0x80) *out += char(cp);
                else if (cp < 0x800) *out += char(0xc0 | cp >> 6), *out += char(0x80 | (cp & 0x3f));
                else *out += char(0xe0 | cp >> 12), *out += char(0x80 | (cp >> 6 & 0x3f)), *out += char(0x80 | (cp & 0x3f));
                break;
            }
            default: *out += json[i]; break;   // " \ /
        }
    }
    return false;
}

// GET /json's pages: each object's "title" and "webSocketDebuggerUrl" (ones without a socket left out).
struct Target {
    std::string title, ws;
};
inline std::vector<Target> Targets(const std::string &json) {
    std::vector<Target> out;
    Target t;
    int depth = 0;
    std::string key;
    bool haveKey = false;
    for (size_t i = 0; i < json.size();) {
        const char c = json[i];
        if (c == '"') {
            std::string s;
            size_t end;
            if (!ReadString(json, i, &s, &end)) break;
            i = end;
            while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
            if (depth == 2 && i < json.size() && json[i] == ':') {
                key = s, haveKey = true, ++i;
            } else if (depth == 2 && haveKey) {
                if (key == "title") t.title = s;
                else if (key == "webSocketDebuggerUrl") t.ws = s;
                haveKey = false;
            }
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
            if (c == '{' && depth == 2) t = Target{};
        } else if (c == '}' || c == ']') {
            if (c == '}' && depth == 2 && !t.ws.empty()) out.push_back(t);
            --depth;
        } else if (c == ',' && depth == 2) {
            haveKey = false;
        }
        ++i;
    }
    return out;
}

// The number after "key": in `json` (the first one). False when there's none.
inline bool NumberAfter(const std::string &json, const std::string &key, double *v) {
    const std::string k = Quote(key) + ":";
    const size_t at = json.find(k);
    if (at == std::string::npos) return false;
    const char *p = json.c_str() + at + k.size();
    char *end = nullptr;
    *v = std::strtod(p, &end);
    return end != p;
}
// Whether `json` is the reply to request `id`.
inline bool IsReply(const std::string &json, int id) {
    return json.find("\"id\":" + std::to_string(id) + ",") != std::string::npos ||
           json.find("\"id\":" + std::to_string(id) + "}") != std::string::npos;
}

inline std::string Evaluate(int id, const std::string &expression) {
    return "{\"id\":" + std::to_string(id) + ",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":" +
           Quote(expression) + ",\"returnByValue\":true}}";
}

// Input.dispatchMouseEvent. x, y: CSS pixels. `button`: "left", "right", "middle" or "none";
// `buttons`: the ones held (1 left, 2 right, 4 middle). deltaY: Wheel's, CSS pixels (+ down).
enum class Mouse { Moved, Pressed, Released, Wheel };
inline std::string MouseEvent(int id, Mouse type, double x, double y, const char *button, int buttons, double deltaY = 0) {
    static const char *const kTypes[] = {"mouseMoved", "mousePressed", "mouseReleased", "mouseWheel"};
    char b[320];
    const bool click = type == Mouse::Pressed || type == Mouse::Released;
    std::snprintf(b, sizeof b,
                  "{\"id\":%d,\"method\":\"Input.dispatchMouseEvent\",\"params\":{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,"
                  "\"button\":\"%s\",\"buttons\":%d%s%s}}",
                  id, kTypes[int(type)], x, y, button, buttons, click ? ",\"clickCount\":1" : "",
                  type == Mouse::Wheel ? (",\"deltaX\":0,\"deltaY\":" + std::to_string(int(deltaY))).c_str() : "");
    return b;
}

}  // namespace cdpwire
