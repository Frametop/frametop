// A self-signed certificate and its key, for a QUIC end that needs one to listen
// (docs/together.md). The certificate names nobody: who a peer is comes from Tailscale and the
// invite, not from this certificate.
#pragma once

#include <string>

namespace selfcert {

// Writes a new P-256 key (PEM, mode 600) and a certificate for it (PEM), valid ten years.
// False with `err` on failure.
bool Make(const std::string &keyPath, const std::string &certPath, std::string &err);

}  // namespace selfcert
