// The tailnet, by tailscaled's word (tailnet.h).
#include "tailnet.h"

#include "json.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace net {
namespace {

constexpr size_t kMostAnswer = 1u << 20;

bool V4(const std::string &address, uint8_t out[4]) { return inet_pton(AF_INET, address.c_str(), out) == 1; }
bool V6(const std::string &address, uint8_t out[16]) { return inet_pton(AF_INET6, address.c_str(), out) == 1; }

// A node's name without its tailnet: "steam-frame.tail1234.ts.net." -> "steam-frame".
std::string FirstLabel(const std::string &name) { return name.substr(0, name.find('.')); }

}  // namespace

bool OnTailnet(const std::string &address) {
    uint8_t a[16];
    if (V4(address, a)) return a[0] == 100 && (a[1] & 0xc0) == 64;
    static const uint8_t kUla[6] = {0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0};
    return V6(address, a) && !std::memcmp(a, kUla, sizeof kUla);
}

bool IsLoopback(const std::string &address) {
    uint8_t a[16];
    if (V4(address, a)) return a[0] == 127;
    static const uint8_t kOne[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return V6(address, a) && !std::memcmp(a, kOne, sizeof kOne);
}

bool HttpBody(const std::string &response, int &status, std::string &body) {
    // "HTTP/1.x NNN ...", headers, an empty line, the body to the end (HTTP/1.0: no chunks)
    if (response.compare(0, 7, "HTTP/1.") != 0 || response.size() < 12 || response[8] != ' ') return false;
    status = 0;
    for (size_t i = 9; i < 12; ++i) {
        if (response[i] < '0' || response[i] > '9') return false;
        status = status * 10 + (response[i] - '0');
    }
    const size_t end = response.find("\r\n\r\n");
    if (end == std::string::npos) return false;
    // a body in chunks would need a reader this doesn't have: refuse it, don't misread it
    std::string head = response.substr(0, end);
    for (char &c : head) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (head.find("\r\ntransfer-encoding:") != std::string::npos) return false;
    body = response.substr(end + 4);
    return true;
}

bool ReadSelf(const std::string &body, std::string &address, std::string &name) {
    json::Value v;
    if (!json::Parse(body, v) || v["BackendState"].text() != "Running") return false;
    const json::Value &self = v["Self"];
    address.clear();
    for (const json::Value &ip : self["TailscaleIPs"].items) {
        uint8_t a[4];
        if (address.empty() && V4(ip.text(), a) && OnTailnet(ip.text())) address = ip.text();
    }
    name = self["HostName"].text();
    if (name.empty()) name = FirstLabel(self["DNSName"].text());
    return !address.empty();
}

bool ReadWho(const std::string &body, session::Who &who) {
    json::Value v;
    if (!json::Parse(body, v) || v["Node"].type != json::Value::kObject) return false;
    who.login = v["UserProfile"]["LoginName"].text();
    who.name = v["Node"]["ComputedName"].text();
    if (who.name.empty()) who.name = FirstLabel(v["Node"]["Name"].text());
    return !who.login.empty();
}

Tailscale::Tailscale(std::string socket) : socket_(std::move(socket)) {
    if (!socket_.empty()) return;
    if (const char *env = std::getenv("FT_TAILSCALED_SOCK")) {
        socket_ = env;
        return;
    }
    for (const char *path : {"/run/tailscale/tailscaled.sock", "/run/host/run/tailscale/tailscaled.sock", "/var/run/tailscale/tailscaled.sock"})
        if (access(path, F_OK) == 0) {
            socket_ = path;
            return;
        }
}

bool Tailscale::Get(const std::string &path, std::string &body) {
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (socket_.empty() || socket_.size() >= sizeof addr.sun_path) return false;
    std::memcpy(addr.sun_path, socket_.c_str(), socket_.size());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const timeval wait = {2, 0};   // it is a local daemon: an answer takes milliseconds
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof wait);
    const std::string ask = "GET " + path + " HTTP/1.0\r\nHost: local-tailscaled.sock\r\n\r\n";
    std::string response;
    bool ok = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0 && send(fd, ask.data(), ask.size(), MSG_NOSIGNAL) == ssize_t(ask.size());
    char buf[8192];
    while (ok) {
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n == 0) break;
        ok = n > 0 && response.size() + size_t(n) <= kMostAnswer;
        if (ok) response.append(buf, size_t(n));
    }
    close(fd);
    int status = 0;
    return ok && HttpBody(response, status, body) && status == 200;
}

bool Tailscale::Self(session::Who &who, std::string &address) {
    std::string body, name;
    if (!Get("/localapi/v0/status?peers=false", body) || !ReadSelf(body, address, name)) return false;
    if (!WhoIs(address, who)) return false;
    if (!name.empty()) who.name = name;
    return true;
}

bool Tailscale::WhoIs(const std::string &address, session::Who &who) {
    // Only an address goes into the question, as tailscale would write it.
    if (!OnTailnet(address)) return false;
    std::string body;
    return Get("/localapi/v0/whois?addr=" + address, body) && ReadWho(body, who);
}

}  // namespace net
