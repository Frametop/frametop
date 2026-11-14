// The address a call goes to: a host's name looked up, and only an address the caller allows
// taken (for a session, one on the tailnet). An address written as a number is checked the
// same way, without asking anyone. A name asks the resolver and waits for it, which is why
// the guests of a room are called by address and only a room's owner by name.
#pragma once

#include <functional>
#include <string>

namespace net {

// The first address of `host` that `allowed` takes, as a number: an IPv4 one before an IPv6
// one, so both ends of a link write an address the same way. "" when there is none.
std::string Lookup(const std::string &host, const std::function<bool(const std::string &address)> &allowed);

}  // namespace net
