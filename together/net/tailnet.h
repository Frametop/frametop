// The tailnet, as a session asks about it (session::Tailnet): who is at an address, by
// tailscaled's word. tailscaled answers on its local socket (the LocalAPI: `status` and
// `whois`), which any user of this machine may read. Nothing here talks to the network.
//
// Loopback is the tailnet of a test: everyone at a loopback address is someone, and nobody else
// is anybody.
#pragma once

#include "../session/session.h"

#include <string>

namespace net {

// An address tailscale hands out: 100.64.0.0/10 or fd7a:115c:a1e0::/48, written as a number.
bool OnTailnet(const std::string &address);
// 127.0.0.0/8 or ::1, written as a number.
bool IsLoopback(const std::string &address);

// The parts of tailscaled's answers a session uses. False when the answer isn't what it should be.
bool HttpBody(const std::string &response, int &status, std::string &body);
// `status`: this node's first IPv4 address and its name. False while Tailscale isn't running.
bool ReadSelf(const std::string &body, std::string &address, std::string &name);
// `whois`: the login of the node's owner, and the node's name.
bool ReadWho(const std::string &body, session::Who &who);

class Tailscale : public session::Tailnet {
public:
    // `socket`: tailscaled's, or "" for the usual places (FT_TAILSCALED_SOCK, /run/tailscale,
    // and the host's under /run/host when in a container).
    explicit Tailscale(std::string socket = "");
    bool Self(session::Who &who, std::string &address) override;
    bool WhoIs(const std::string &address, session::Who &who) override;
    bool MayCall(const std::string &address) override { return OnTailnet(address); }

private:
    bool Get(const std::string &path, std::string &body);
    std::string socket_;
};

class Loopback : public session::Tailnet {
public:
    explicit Loopback(std::string name) : name_(std::move(name)) {}
    bool Self(session::Who &who, std::string &address) override { return who.login = name_ + "@loopback", who.name = name_, address = "127.0.0.1", true; }
    bool WhoIs(const std::string &address, session::Who &who) override { return who.login = "someone@loopback", who.name = "loopback", IsLoopback(address); }
    bool MayCall(const std::string &address) override { return IsLoopback(address); }

private:
    std::string name_;
};

}  // namespace net
