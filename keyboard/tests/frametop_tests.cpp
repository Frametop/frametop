// FrametopLink and FrametopSink against a private stand-in relay (an abstract name of this
// test's own), so nothing reaches Frametop's relay or a desktop.
#include "framekeyboard/frametop.hpp"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace framekeyboard;
namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}
struct Steam : KeySink {
    std::vector<std::pair<int, int>> keys;
    std::vector<std::string> text;
    void send(int code, int value) override { keys.emplace_back(code, value); }
    bool text_available() override { return true; }
    bool commit_text(const std::string& value) override {
        text.push_back(value);
        return true;
    }
};
sockaddr_un named(const std::string& name, socklen_t& length) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return address;
}
// The stand-in relay: bound to `name`, it remembers who said hello last.
struct Relay {
    int fd{-1};
    sockaddr_un peer{};
    socklen_t peer_length{};
    explicit Relay(const std::string& name) {
        fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        socklen_t length{};
        const auto address = named(name, length);
        require(fd >= 0 && bind(fd, reinterpret_cast<const sockaddr*>(&address), length) == 0,
                "bind stand-in relay");
    }
    ~Relay() { close(fd); }
    std::vector<std::string> take() {
        std::vector<std::string> result;
        for (;;) {
            char data[512];
            sockaddr_un from{};
            socklen_t length = sizeof(from);
            const auto count = recvfrom(fd, data, sizeof(data), MSG_DONTWAIT,
                                        reinterpret_cast<sockaddr*>(&from), &length);
            if (count < 0) {
                return result;
            }
            peer = from, peer_length = length;
            result.emplace_back(data, static_cast<std::size_t>(count));
        }
    }
    void say(const std::string& text) {
        require(sendto(fd, text.data(), text.size(), 0, reinterpret_cast<const sockaddr*>(&peer),
                       peer_length) == static_cast<ssize_t>(text.size()),
                "stand-in relay send");
    }
};
} // namespace

int main() {
    try {
        const std::string name = "framekeyboard-test-relay-" + std::to_string(getpid());
        double now = 100;
        Relay relay(name);
        FrametopLink link(name, [&] { return now; });
        Steam steam;
        FrametopSink sink(link, steam);

        // Hello on the first poll; no word yet, so typing goes to Steam.
        link.poll();
        require(relay.take() == std::vector<std::string>{"kbhello 0"}, "hello on first poll");
        require(!link.fresh() && !link.desktop() && !link.surface(), "no word: not fresh");
        sink.send(KEY_A, 1);
        sink.send(KEY_A, 0);
        require(steam.keys.size() == 2, "no word: keys go to Steam");

        // The relay says the desktop has typing and shows.
        relay.say("kb 1 1");
        link.poll();
        require(link.fresh() && link.desktop() && link.surface(), "word from the relay");
        sink.send(KEY_B, 1);
        require(sink.commit_text("héllo"), "desktop text accepted");
        sink.send(KEY_B, 2); // repeats stay with the receiver
        auto got = relay.take();
        require(got == std::vector<std::string>{"kbkey 48 1", "kbtext héllo"},
                "desktop typing to relay");
        require(steam.keys.size() == 2 && steam.text.empty(), "nothing to Steam meanwhile");

        // Typing moves to Steam with B held: B is still released on the desktop.
        relay.say("kb 0 1");
        link.poll();
        require(!link.desktop() && link.surface(), "typing moved to Steam");
        sink.send(KEY_B, 0);
        sink.send(KEY_C, 1);
        sink.send(KEY_C, 0);
        require(sink.commit_text("x"), "Steam text accepted");
        got = relay.take();
        require(got == std::vector<std::string>{"kbkey 48 0"}, "release where it was pressed");
        require(steam.keys.size() == 4 && steam.keys[2] == std::pair{KEY_C, 1} && steam.text.size() == 1,
                "C and text to Steam");

        // Requests.
        relay.say("kb show");
        link.poll();
        require(link.take_request() == FrametopLink::Request::Show, "show");
        require(link.take_request() == FrametopLink::Request::None, "taken once");
        relay.say("kb toggle");
        link.poll();
        require(link.take_request() == FrametopLink::Request::Toggle, "toggle");
        relay.say("kb hide");
        link.poll();
        require(link.take_request() == FrametopLink::Request::Hide, "hide");

        // A fingertip's pokes, from ft-screens through the relay.
        relay.say("kb poke right press 0.1200 1.0500 -0.6500");
        relay.say("kb poke left end");
        relay.say("kb poke left up");
        relay.say("kb poke right hover");        // a hover says where
        relay.say("kb poke middle hover 0 0 0"); // no such hand
        relay.say("kb poke right type 0 0 0");   // no such verb
        relay.say("kb poke right move 0 0 nan");
        relay.say("kb poke right move 0 0 0 junk");
        link.poll();
        auto pokes = link.take_pokes();
        require(pokes.size() == 3, "three good pokes, the rest dropped");
        require(pokes[0].hand == 1 && pokes[0].verb == HandPoke::Verb::Press && pokes[0].point &&
                    (*pokes[0].point)[1] == 1.05,
                "the press and its point");
        require(pokes[1].hand == 0 && pokes[1].verb == HandPoke::Verb::End && !pokes[1].point, "an end");
        require(pokes[2].verb == HandPoke::Verb::Up && !pokes[2].point, "an up without a point");
        require(link.take_pokes().empty(), "taken once");

        // Silence: the word goes stale, and typing goes back to Steam.
        relay.say("kb 1 1");
        link.poll();
        require(link.desktop(), "desktop again");
        now += FrametopLink::stale_after + .1;
        require(!link.fresh() && !link.desktop() && !link.surface(), "stale after silence");
        link.poll();
        require(relay.take() == std::vector<std::string>{"kbhello 0"}, "hello again after a second");
        link.set_open(true);
        link.poll();
        require(relay.take() == std::vector<std::string>{"kbhello 1"}, "hello at once when it opens");
        link.set_open(true);
        link.poll();
        require(relay.take().empty(), "no change, no hello within the second");

        // Another name can't speak for the relay.
        {
            const int other = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            socklen_t length{};
            const auto impostor = named(name + "-impostor", length);
            require(bind(other, reinterpret_cast<const sockaddr*>(&impostor), length) == 0,
                    "bind impostor");
            const std::string text = "kb 1 1";
            sendto(other, text.data(), text.size(), 0, reinterpret_cast<const sockaddr*>(&relay.peer),
                   relay.peer_length);
            close(other);
            link.poll();
            require(!link.fresh(), "impostor ignored");
        }
        std::cout << "frametop: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "frametop: " << error.what() << '\n';
        return 1;
    }
}
