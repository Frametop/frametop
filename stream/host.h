// The host side of a Frametop remote display (docs/remote-displays.md): one Moonlight client
// per display, paired with a Vibepollo host through its Web UI API token, or with a Sunshine
// instance through its Web UI login, and the requests that open and close the display's stream.
// Shared by ft-stream and the spike's ft-streamtest.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

extern "C" {
#include "client.h"
#include "errors.h"
#include "http.h"
}

namespace ftstream {

constexpr int kRemoteMonitor = 2147483505, kDisconnectMonitor = 2147483502;  // Vibepollo's remote_session.h
// frametop-vibepollo: stream an existing display, named by the launch argument frametopDisplay.
constexpr int kFrametopDisplay = 2147483521;
constexpr const char *kPrimaryApp = "Frametop primary display";
constexpr int kGameStreamPort = 47989;  // a host's HTTP port; its Web UI is on the next one

// ~/.local/share/frametop-stream/ID: one client's certificate, key and unique id.
std::string KeyDir(const std::string &id);
// The host's API token: ~/.local/share/frametop-stream/hosts/HOST.token.
std::string TokenFile(const std::string &host);
// HOST[:PORT] (Moonlight's form) -> HOST and PORT (0 without one); false if PORT isn't a port.
bool SplitPort(std::string *host, int *port);

// The host's Web UI API, on the port after its HTTP port: with a scoped token (Vibepollo), or
// with the Web UI login (plain Sunshine, which has no tokens).
struct WebApi {
    std::string base, token, login, pin;  // login: "USER:PASSWORD"; pin: the Web UI's public key,
                                          // "sha256//BASE64" (curl's form)
    bool Load(const std::string &host, const std::string &file);
    // The login as two lines (user name, password) from `in`, for the instance on `port`.
    bool LoadLogin(const std::string &host, int port, FILE *in);
    // The HTTP status (or -1), with the response's JSON in *out.
    long Call(const char *method, const std::string &path, const nlohmann::json *body, nlohmann::json *out);
    // Paired clients by host id: name and permissions.
    bool Clients(std::map<std::string, std::pair<std::string, uint32_t>> *clients);
    // Lets a client launch apps and use the mouse and keyboard.
    bool Grant(const std::string &uuid, const std::string &name);
};

// One client (one display) of one host.
struct Client {
    std::string host, id, dir, uniqueId;
    int port = 0;  // its HTTP port; 0: 47989
    SERVER_DATA server{};
    std::string display;  // a frametopDisplay launch's device id

    // Makes the key directory and a unique id if needed, then asks the host about itself.
    bool Connect(const std::string &host, const std::string &id, int port = 0);
    // Pairs this client: with the token, by sending its own PIN through the API and then
    // granting it launch, mouse and keyboard; with the login, by approving its own pairing
    // request with its PIN (Sunshine has no permissions to grant); with neither, by printing
    // the PIN to type in.
    bool Pair(WebApi *api);
    bool PairByLogin(WebApi &api, char *pin);  // gs_pair takes a char *
    bool GrantByName(WebApi &api);
    bool Unpair();
    // The app id for primary, monitor, display:DEVICE, a number, or an app's name; 0 if none.
    int FindApp(const std::string &want);
    // /launch (or /resume) for appId, keeping the status code: Vibepollo answers 503 while a
    // Remote Monitor is being set up.
    int Launch(STREAM_CONFIGURATION &cfg, int appId, bool resume, std::string *message);
    // libgamestream's connection is dropped after every request (see host.cpp).
    void DropConnection();
};

}  // namespace ftstream
