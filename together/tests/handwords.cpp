// A stand-in for ft-peerpanel's hand, for panelhand-test.py: ft-screens' words, a line each on
// its standard input, go through the same PanelHand and HandSender (together/local) to an
// ft-together's local socket, as ft-peerpanel's loop sends them. No decoder, no ft-screens.
//
//   handwords PANEL SOCKET WIDTH HEIGHT
//
// It ends when its input does, as ft-peerpanel does when ft-screens goes: the hand lets go.
#include "../local/handsend.h"

#include <poll.h>

#include <cstdlib>
#include <ctime>

static double Now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    if (argc != 5) return std::fprintf(stderr, "usage: handwords PANEL SOCKET WIDTH HEIGHT\n"), 2;
    local::PanelHand hand{uint8_t(std::atoi(argv[1]))};
    local::HandSender toRoom(argv[2]);
    local::PanelHand::Out said;
    hand.Size(std::atoi(argv[3]), std::atoi(argv[4]));
    std::string waiting;
    for (bool open = true; open;) {
        pollfd p = {0, POLLIN, 0};
        const double due = hand.Wait(Now());
        if (poll(&p, 1, due < 0 || due > 0.1 ? 100 : int(due * 1000) + 1) > 0) {
            char buf[512];
            const ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) open = false;
            else waiting.append(buf, size_t(n));
        }
        for (size_t end; (end = waiting.find('\n')) != std::string::npos; waiting.erase(0, end + 1)) hand.Word(waiting.substr(0, end), Now(), said);
        hand.Tick(Now(), said);
        toRoom.Send(said);
    }
    hand.End(Now(), said);
    toRoom.Send(said);
    return 0;
}
