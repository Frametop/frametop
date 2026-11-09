// ft-stream: one remote display for ft-screens (docs/remote-displays.md). A Moonlight client
// (moonlight-common-c, and libgamestream for pairing and launching) for one display of a
// Vibepollo host, or of a Sunshine instance (one per display, each on its own port: the Mac). It decodes the stream on the Frame's iris decoder and converts each picture
// into a ring of three RGBA buffers (iris.h), which ft-screens shows as a panel like any
// screen of the desktop: its controls, layout, profiles, curve, pins and hand cutouts. Each
// display is its own client with its own certificate (--id), because Vibepollo ties a Remote
// Monitor to the client that opened it, one stream per client.
//
//   ft-stream pair HOST --id NAME [--token FILE | --login]
//                    (pairs with the host's API token, with its Web UI login read from stdin as
//                    two lines, user name and password (Sunshine), or with a PIN typed in there)
//   ft-stream grant HOST --id NAME [--token FILE]    (permissions for a client paired by PIN)
//   ft-stream unpair HOST --id NAME
//   ft-stream monitors HOST [--token FILE]           (the host's monitors, as Vibepollo lists them)
//   ft-stream release HOST --id NAME                 (Vibepollo's "Disconnect Monitor" for this client)
//   ft-stream stream HOST --id NAME --fd N --app display:DEVICE|monitor|primary|ID|NAME
//                    [--size 2560x1440] [--fps 60] [--bitrate kbps]
//
// HOST may be HOST:PORT, a host whose HTTP port isn't 47989 (a Mac's second Sunshine instance
// is on 48089). Keys live in ~/.local/share/frametop-stream/NAME, the host's token in
// ~/.local/share/frametop-stream/hosts/HOST.token (see host.h), named without the port, or
// HOST-PORT.token for an instance with a token of its own (a host's Frametop windows).
//
// --app window:ADDRESS streams one app window of a host's Frametop windows instance (a
// Vibepollo in the host's desktop session): it asks the host to put that window on an output of
// its own at --size (POST /api/frametop/windows/open; again on every start, so a window that
// went back comes out again) and streams that output as a display.
//
// "stream" is started by ft-screens (screens/remote.c), which gives it one end of a
// SOCK_SEQPACKET socket pair as fd N. Messages are text, one per packet.
//   From ft-screens:
//     modifiers M...        the DRM format modifiers SteamVR imports ABGR8888 with (hex), first
//     release I             ring buffer I is off the panel: it may be drawn into again
//     attention focused|view|hidden
//                           hidden: every frame is still decoded (so it's back at once), and
//                           kHiddenFps of them shown
//     move X Y              the pointer, in stream pixels from the top left
//     button CODE 1|0       a linux BTN_* code, pressed or released
//     scroll DX DY          wheel notches (positive DY: down)
//     key CODE 1|0          a linux KEY_* code, pressed or released
//     focus                 typing went to this screen (a click or a spin to it): the shared
//                           clipboard sends the desktop's text
//     blur                  typing went to another screen: the keys held here come up,
//                           gamepad mode's pad goes, and the shared clipboard brings the host's
//                           text back
//     pad B LT RT LX LY RX RY
//                           gamepad mode (input/vrpad.py): the Frame controllers as an Xbox pad,
//                           B its XInput buttons (hex), triggers 0-255, sticks -32767..32767 (up
//                           positive). The host gets a pad on the first; it stays till "pad off"
//     pad off               gamepad mode ended or went to another screen: the pad goes
//     quit
//   To ft-screens:
//     state connecting|live|lost [TEXT]   (live via dongle|network: which way it came, see Route)
//     buffers W H FORMAT MODIFIER OFFSET STRIDE    with the three buffers' dmabuf fds
//                           (SCM_RIGHTS), single plane, FORMAT and MODIFIER in hex
//     frame I               ring buffer I holds a new picture (the GPU is done with it)
// It exits when ft-screens closes its end. A Remote Monitor is released then ("Disconnect
// Monitor"); an existing display holds nothing on the host.
//
// The host's sound plays here, and with "mic": true on its host in the layout (Frametop Remote
// Displays: Send my microphone) the Frame's microphone goes back to the host's virtual one; see
// Audio and Mic. With "input": true (Use this computer's keyboard and mouse) the host's own
// keyboard and mouse come to the input relay while the headset is worn; see HostInput. With
// "clipboard": true (Share the clipboard) text copied on either side can be pasted on the other;
// see SharedClipboard.
//
// Runs in the dev container. Build: stream/build.sh.
#include "iris.h"

#include "../conf/ftsock.h"
#include "clipboard.h"
#include "host.h"
#include "hostinput.h"

#include <netdb.h>
#include <poll.h>
#include <spawn.h>
#include <opus/opus.h>
#include <opus/opus_multistream.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <linux/input-event-codes.h>

#include "input/keyboard.h"  // moonlight-embedded: linux KEY_* -> Windows virtual-key codes

namespace {

using namespace ftstream;

// One frame from moonlight-common-c's decoder thread, waiting for the main loop.
struct Unit {
    std::vector<uint8_t> data;
};

std::mutex g_mu;
std::deque<Unit> g_units;
int g_wake = -1;
std::atomic<bool> g_hidden{false}, g_waitIdr{false};
std::atomic<int> g_ended{0};  // connection terminated: error code + 1
// Seconds the way out may take before ft-stream ends itself (Stream): longer than ft-screens'
// FT_REMOTE_QUIT_MS (20 s), so while ft-screens runs, its SIGKILL comes first.
constexpr unsigned kExitWatchdogS = 25;
// For the log's report: frames received and decoded, bytes, decode time.
std::atomic<int> g_received{0}, g_skipped{0};
std::atomic<uint64_t> g_bytes{0};
// Out of sight (attention hidden), a panel still shows this many frames a second.
constexpr int kHiddenFps = 10;

// The host's sound. Every display's stream carries it, the same for all of them, so one
// stream per host plays it: the one holding $XDG_RUNTIME_DIR/frametop-audio-HOST.lock (the
// others try again every few seconds, so another takes over when that one ends). It plays
// through PipeWire's PulseAudio server; the host keeps playing it too (localAudioPlayMode).
struct Audio {
    std::atomic<bool> on{false};
    std::string name;  // the playback stream's name in the mixer
    int lock = -1;
    int64_t tryAt = 0;
    OpusMSDecoder *dec = nullptr;
    pa_simple *pa = nullptr;
    bool paFailed = false;
    int rate = 48000, channels = 2, samples = 240;
    std::vector<int16_t> pcm;

    void Claim(const std::string &host, int64_t now) {
        if (on || now < tryAt) return;
        tryAt = now + 3'000'000'000;
        if (lock < 0) {
            const char *dir = std::getenv("XDG_RUNTIME_DIR");
            const std::string path = std::string(dir && *dir ? dir : "/tmp") + "/frametop-audio-" + host + ".lock";
            lock = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        }
        if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0) {
            on = true;
            std::printf("audio: this stream plays %s's sound\n", host.c_str());
            std::fflush(stdout);
        }
    }
};
Audio g_audio;
std::string g_host;  // the host's own address, whichever way the stream goes (Route)

int AudioInit(int, const POPUS_MULTISTREAM_CONFIGURATION opus, void *, int) {
    int err = 0;
    g_audio.dec = opus_multistream_decoder_create(opus->sampleRate, opus->channelCount, opus->streams,
                                                  opus->coupledStreams, opus->mapping, &err);
    if (!g_audio.dec) return std::fprintf(stderr, "audio: opus: %s\n", opus_strerror(err)), -1;
    g_audio.rate = opus->sampleRate, g_audio.channels = opus->channelCount, g_audio.samples = opus->samplesPerFrame;
    g_audio.pcm.resize(size_t(opus->samplesPerFrame * opus->channelCount));
    return 0;
}

void AudioCleanup() {
    if (g_audio.pa) pa_simple_free(g_audio.pa);
    if (g_audio.dec) opus_multistream_decoder_destroy(g_audio.dec);
    g_audio.pa = nullptr, g_audio.dec = nullptr;
}

// On moonlight-common-c's audio thread; a lost packet comes as nullptr (Opus fills it in).
void AudioPlay(char *data, int length) {
    if (!g_audio.on || !g_audio.dec) return;
    if (!g_audio.pa) {
        if (g_audio.paFailed) return;
        const pa_sample_spec spec = {PA_SAMPLE_S16LE, uint32_t(g_audio.rate), uint8_t(g_audio.channels)};
        pa_buffer_attr attr;
        attr.maxlength = uint32_t(-1), attr.prebuf = uint32_t(-1), attr.minreq = uint32_t(-1), attr.fragsize = uint32_t(-1);
        attr.tlength = uint32_t(pa_usec_to_bytes(40'000, &spec));  // about 40 ms queued: little delay over the picture
        int err = 0;
        g_audio.pa = pa_simple_new(nullptr, "Frametop", PA_STREAM_PLAYBACK, nullptr, g_audio.name.c_str(), &spec, nullptr,
                                   &attr, &err);
        if (!g_audio.pa) {
            g_audio.paFailed = true;
            std::fprintf(stderr, "audio: can't play: %s\n", pa_strerror(err));
            return;
        }
    }
    const int n = opus_multistream_decode(g_audio.dec, reinterpret_cast<unsigned char *>(data), length, g_audio.pcm.data(),
                                          g_audio.samples, 0);
    int err = 0;
    if (n > 0) pa_simple_write(g_audio.pa, g_audio.pcm.data(), size_t(n * g_audio.channels) * sizeof(int16_t), &err);
}

// The Frame's microphone, to the host's virtual microphone: Steam Streaming Microphone on
// Windows (Frametop's host build; others ignore it, and so does its Linux service for now).
// Sent by the stream that plays the host's sound (Audio), so a host gets it once however many
// of its displays stream, while its host has "mic": true in the layout. Mono Opus at 48 kHz in 20 ms
// packets on the encrypted control stream (LiSendMicrophoneOpusData, moonlight-mic.patch), never
// in the clear. It records PipeWire's default source, which on the Frame runs through echo
// cancelling, so the host's sound playing here doesn't go back into a call.
struct Mic {
    std::string name;  // the recording stream's name in the mixer
    std::mutex mu;     // held while a packet goes out; Stop takes it, so none goes after
    bool run = false;
    std::atomic<bool> done{true};
    std::atomic<bool> refused{false};  // the control stream isn't encrypted: never for this stream
    std::atomic<int> sent{0};
    int64_t retryAt = 0;  // a recording that ended by itself (no source) starts again after 30 s
    std::thread thread;

    bool On() {
        std::lock_guard<std::mutex> lock(mu);
        return run;
    }

    void Want(bool on) {
        if (on == On() || (on && refused)) return;
        if (!on) return Stop();
        if (MonoNs() < retryAt) return;
        retryAt = MonoNs() + 30'000'000'000;
        if (thread.joinable()) thread.join();  // a loop that ended by itself
        {
            std::lock_guard<std::mutex> lock(mu);
            run = true;
        }
        done = false;
        thread = std::thread(&Mic::Loop, this);
    }

    // Afterwards no packet goes out, so the connection can stop. A recording stuck in
    // pa_simple_read (it can't be interrupted) is left behind rather than waited for.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!run && !thread.joinable()) return;
            run = false;
        }
        for (int i = 0; i < 50 && !done; ++i) usleep(10'000);
        if (!thread.joinable()) return;
        if (done) thread.join();
        else thread.detach(), std::printf("mic: the recording didn't end, left behind\n");
        std::printf("mic: off\n");
    }

    void Loop() {
        constexpr int kSamples = 960;  // 20 ms at 48 kHz
        const pa_sample_spec spec = {PA_SAMPLE_S16LE, 48000, 1};
        pa_buffer_attr attr;
        attr.maxlength = uint32_t(-1), attr.prebuf = uint32_t(-1), attr.minreq = uint32_t(-1), attr.tlength = uint32_t(-1);
        attr.fragsize = uint32_t(kSamples * sizeof(int16_t));
        int err = 0;
        pa_simple *pa = pa_simple_new(nullptr, "Frametop", PA_STREAM_RECORD, nullptr, name.c_str(), &spec, nullptr, &attr, &err);
        OpusEncoder *enc = pa ? opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err) : nullptr;
        if (!pa) std::fprintf(stderr, "mic: can't record: %s\n", pa_strerror(err));
        else if (!enc) std::fprintf(stderr, "mic: opus: %s\n", opus_strerror(err));
        if (enc) {
            opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
            opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));  // speech at 48 kHz mono: a few % of a core
            opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));  // the next packet carries a lost one
            opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));
            std::printf("mic: sending the default source to the host\n");
            std::fflush(stdout);
        }
        int16_t pcm[kSamples];
        unsigned char out[LI_MICROPHONE_MAX_OPUS_BYTES];
        while (enc) {
            if (pa_simple_read(pa, pcm, sizeof pcm, &err) < 0) {
                std::fprintf(stderr, "mic: recording ended: %s\n", pa_strerror(err));
                break;
            }
            const int n = opus_encode(enc, pcm, kSamples, out, sizeof out);
            std::lock_guard<std::mutex> lock(mu);
            if (!run) break;
            if (n <= 0) continue;
            const int r = LiSendMicrophoneOpusData(out, n);
            if (r == 0) ++sent;
            else if (r == -1) {
                refused = true;
                std::printf("mic: not sent: this host's control stream isn't encrypted\n");
                break;
            }
        }
        if (enc) opus_encoder_destroy(enc);
        if (pa) pa_simple_free(pa);
        {
            std::lock_guard<std::mutex> lock(mu);
            run = false;
        }
        done = true;
    }
};
Mic g_mic;

bool HostFlag(const std::string &host, const char *key);

// A test's file (hostinput::TestForbidding) allows a host's input only with "allow_host_input": true.
bool AllowsHostInput(const std::string &text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    const auto allow = j.find("allow_host_input");
    return allow != j.end() && allow->is_boolean() && allow->get<bool>();
}

// The host's own keyboard and mouse, lent to Frametop (docs/host-input.md) by a Linux host with
// Frametop's Vibepollo build. One stream per host holds them: the one holding
// $XDG_RUNTIME_DIR/frametop-input-HOST.lock. While "input": true is on its layout entry, the
// stream is live, the pointer helper says the headset is worn and the relay isn't paused, it
// renews a lease every 500 ms (LiSendFrametopHostInputLease, moonlight-hostin.patch). When the
// switch goes off or the headset comes off it releases, which also lifts the host's escape (both
// Shifts and Escape there); anything else stopping the lease drops it and keeps the escape
// (hostinput::Decide). So does a test faking the wearer or a verification run under way without
// leave to take the host's input, which it checks for before every lease: the lock alone left a gap, a host given the switch
// while its stream held the lock leasing until the testbench's guard let the wearer go. The host
// answers with records (stream/hostinput.h) on the control stream's receive thread, and
// hostinput::Relay turns them into the relay's host devices.
struct HostInput {
    std::string host, id;  // the host's address, and its id for the relay and the lock
    std::string mark;      // where the host's escape is kept until a release lifts it (hostinput::EscapeMark)
    int sock = -1;         // datagrams to the relay and the pointer helper, and their replies
    bool switchOn = false, locked = false, refusedSaid = false;
    int lock = -1;
    std::vector<std::string> testDirs;  // where the tests keep their files
    std::string testSaid;               // the test file the log last said stops the leases
    int64_t checkAt = 0, lockAt = 0, askAt = 0, leaseAt = 0;
    hostinput::Answer worn, paused;
    // What Tick last wanted; a change to kRelease is an edge. A stream starting with the switch off
    // sends none; one starting with the headset off sends one at the helper's first answer (the
    // stream isn't live before it). An edge's release is owed until it went, and no engage goes
    // ahead of it; so is one for an escape the headset came off and went on since (TakeAnswers).
    hostinput::Want last = hostinput::Want::kRelease;
    bool releaseOwed = false;
    uint32_t seq = 0;
    // With the receive thread.
    std::mutex mu;
    bool leasing = false;
    bool escaped = false;  // the host said so: a release lifts it even once the lease has stopped
    int64_t escapedAt = 0; // when it was heard, here or by an earlier stream (CLOCK_BOOTTIME ns)
    hostinput::Relay relay{[this](const std::string &msg) { return Send(kRelay, msg, true); }};

    static constexpr const char *kRelay = FT_SOCK_RELAY, *kHelper = FT_SOCK_POINTER_HELPER;

    static int64_t BootNs() {
        timespec t;
        clock_gettime(CLOCK_BOOTTIME, &t);
        return int64_t(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
    }

    // To the relay or the pointer helper. The relay's host messages wait for room (up to the
    // socket's 100 ms send timeout): a lost one could be a key-up.
    bool Send(const char *to, const std::string &msg, bool wait = false) const {
        return ft_sock_send(sock, to, msg.data(), msg.size(), wait ? 0 : MSG_DONTWAIT) == 0;
    }

    // A record from the host, on the control stream's receive thread.
    void Got(const unsigned char *data, int length) {
        const hostinput::Record r = hostinput::Parse(data, length);
        std::lock_guard<std::mutex> lock(mu);
        // Even once the lease stopped: an escape said just after a drop is still lifted by a release.
        // It's kept for the next stream until then: the host keeps it across streams.
        if (r.type == hostinput::Record::kState && (r.engaged || r.text == "escape")) {
            if (r.engaged && escaped) hostinput::WriteMark(mark, 0), escapedAt = 0;
            else if (!r.engaged && !escaped) escapedAt = BootNs(), hostinput::WriteMark(mark, escapedAt);
            escaped = !r.engaged;
        }
        if (!leasing) return;
        const std::string said = relay.Take(r);
        if (!said.empty()) std::printf("hostinput: %s\n", said.c_str()), std::fflush(stdout);
    }

    int TakeEvents() {
        std::lock_guard<std::mutex> lock(mu);
        return relay.TakeEvents();
    }

    // An escape an earlier stream heard and no release has lifted yet.
    void Recall() {
        const int64_t at = hostinput::ReadMark(mark);
        std::lock_guard<std::mutex> lock(mu);
        if (at && (!escaped || at < escapedAt)) escaped = true, escapedAt = at;
    }

    void Open(const std::string &address) {
        host = address, id = hostinput::RelayId(address);
        mark = hostinput::EscapeMark(std::getenv("XDG_RUNTIME_DIR"), id);
        Recall();
        testDirs = hostinput::TestDirs(std::getenv("XDG_RUNTIME_DIR"), getuid());
        sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        const timeval wait{0, 100'000};
        const int on = 1;
        // An address of our own (autobind), for the replies, each with its sender's credentials
        // and the time it was sent (hostinput::GivenAt).
        if (sock >= 0 && (bind(sock, (const sockaddr *)&a, sizeof(sa_family_t)) < 0 ||
                          setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof wait) < 0 ||
                          setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &on, sizeof on) < 0 ||
                          setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof on) < 0))
            close(sock), sock = -1;
    }

    // A release or a drop to the host. A release that went lifts any escape and pays an edge's.
    bool Send(bool release) {
        const auto msg = release ? hostinput::Release() : hostinput::Drop();
        if (LiSendFrametopHostInputLease(msg.data(), int(msg.size())) != 0) return false;
        if (release) {
            std::lock_guard<std::mutex> lock(mu);
            escaped = false, escapedAt = 0, releaseOwed = false;
            hostinput::WriteMark(mark, 0);
        }
        std::printf("hostinput: %s %s's keyboard and mouse\n", release ? "released" : "dropped", host.c_str());
        std::fflush(stdout);
        return true;
    }

    // Give the host its keyboard and mouse back now: a release also lifts its escape, a drop
    // keeps it. A release goes even with no lease when an edge's is owed, or while the host may
    // still be escaped; one that couldn't go is tried again on the next tick.
    void Stop(hostinput::Want how) {
        const bool release = how == hostinput::Want::kRelease;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!leasing && !(release && (escaped || releaseOwed))) return;
            leasing = false;
            relay.Stop();
        }
        Send(release);
    }

    void Apply(hostinput::Want want) {
        if (want == hostinput::Want::kRelease && last != hostinput::Want::kRelease) releaseOwed = true;
        last = want;
        if (want != hostinput::Want::kHold) Stop(want);
        else if (releaseOwed) Send(true);  // the headset went back on before it could go
    }

    void Unlock() {
        if (!locked) return;
        flock(lock, LOCK_UN);
        locked = false;
    }

    // The pointer helper's and the relay's answers that came, from this user's processes only, each
    // as of when it was given. USE false: drop them unread.
    void TakeAnswers(int64_t now, bool use) {
        char buf[2048];
        for (;;) {
            iovec io{buf, sizeof buf};
            alignas(cmsghdr) char control[CMSG_SPACE(sizeof(ucred)) + CMSG_SPACE(sizeof(timespec))];
            msghdr m{};
            m.msg_iov = &io, m.msg_iovlen = 1, m.msg_control = control, m.msg_controllen = sizeof control;
            const ssize_t n = recvmsg(sock, &m, MSG_DONTWAIT);
            if (n <= 0) break;
            // Only this user's answers count. Not by address: the helper answers from an unbound socket.
            // One with no send time has no age to go by.
            bool mine = false, stamped = false;
            timespec sent{};
            for (cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
                if (c->cmsg_level != SOL_SOCKET) continue;
                if (c->cmsg_type == SCM_CREDENTIALS) {
                    ucred cred;
                    std::memcpy(&cred, CMSG_DATA(c), sizeof cred);
                    mine = cred.uid == getuid();
                } else if (c->cmsg_type == SCM_TIMESTAMPNS) {
                    std::memcpy(&sent, CMSG_DATA(c), sizeof sent), stamped = true;
                }
            }
            if (!mine || !stamped || !use) continue;
            timespec wall;
            clock_gettime(CLOCK_REALTIME, &wall);
            const int64_t at = hostinput::GivenAt(now, int64_t(wall.tv_sec) * 1'000'000'000 + wall.tv_nsec,
                                                  int64_t(sent.tv_sec) * 1'000'000'000 + sent.tv_nsec);
            const std::string reply(buf, size_t(n));
            if (reply.rfind("ok ", 0) == 0) {
                worn = {reply.find(" worn") != std::string::npos, at};
                // The headset came off and went on again since the escape, with no stream to see it.
                std::lock_guard<std::mutex> lock(mu);
                if (escaped && hostinput::OffOnSince(escapedAt, hostinput::OnAt(reply))) releaseOwed = true;
            } else {
                const nlohmann::json j = nlohmann::json::parse(reply, nullptr, false);
                if (j.is_object() && j.value("t", "") == "pause" && j.contains("paused") && j["paused"].is_boolean())
                    paused = {j["paused"].get<bool>(), at};
            }
        }
    }

    void Tick(int64_t now, bool live) {
        if (sock < 0) return;
        if (now >= checkAt) checkAt = now + hostinput::kStaleNs, switchOn = HostFlag(host, "input");
        if (!switchOn || !live) return Apply(hostinput::Decide(switchOn, live, worn, paused, false, now)), Unlock();
        if (!locked && now >= lockAt) {
            lockAt = now + hostinput::kStaleNs;
            if (lock < 0) {
                const char *dir = std::getenv("XDG_RUNTIME_DIR");
                const std::string path = std::string(dir && *dir ? dir : "/tmp") + "/frametop-input-" + id + ".lock";
                lock = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
            }
            locked = lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0;
            if (locked) std::printf("hostinput: this stream holds %s's keyboard and mouse lock\n", host.c_str()), Recall();
        }
        if (!locked) return;
        if (now >= askAt) askAt = now + 1'000'000'000, Send(kHelper, "gaze ? headset on-at"), Send(kRelay, "pause ?");
        TakeAnswers(now, true);
        // Read every tick, so no lease goes out once a test's file is there.
        timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        const std::string test =
            hostinput::TestForbidding(testDirs, int64_t(wall.tv_sec) * 1'000'000'000 + wall.tv_nsec, AllowsHostInput);
        if (test != testSaid) {
            // The answers so far may be from while that test faked the wearer: ask again.
            worn = hostinput::AfterTest(worn, testSaid, test);
            if (test.empty()) {
                TakeAnswers(now, false), askAt = now;
                std::printf("hostinput: no test stops %s's lease now\n", host.c_str());
            } else std::printf("hostinput: no lease for %s's keyboard and mouse while %s says a test may not take them\n",
                               host.c_str(), test.c_str());
            testSaid = test;
        }
        const hostinput::Want want = hostinput::Decide(switchOn, live, worn, paused, !test.empty(), now);
        Apply(want);
        if (want != hostinput::Want::kHold || releaseOwed) return;
        if (now < leaseAt) return;
        leaseAt = now + hostinput::kLeaseEveryNs;
        bool asking;
        {
            std::lock_guard<std::mutex> lock(mu);
            asking = !leasing;
            if (asking) leasing = true, relay.Start(id, host);  // before the host can answer
        }
        const auto engage = hostinput::Engage(seq++);
        if (LiSendFrametopHostInputLease(engage.data(), int(engage.size())) == -1) {
            // Not an encrypted control stream, or not connected yet: asked again now and then.
            leaseAt = now + hostinput::kStaleNs;
            if (!refusedSaid) std::printf("hostinput: not asked: this host's control stream isn't encrypted, or isn't up yet\n");
            refusedSaid = true;
            std::lock_guard<std::mutex> lock(mu);
            leasing = false;
        } else {
            if (asking) std::printf("hostinput: asking for %s's keyboard and mouse\n", host.c_str());
            refusedSaid = false;
        }
        std::fflush(stdout);
    }
};
HostInput g_hostin;
// --app window:ADDRESS: a Frametop windows instance, which lends no keyboard or mouse. Its stream
// mustn't take the host's input lock (keyed by address, not port) from the stream of one of the
// host's displays: a window panel that started first after a desktop restart held it, its
// instance answered "released: service", and oma7's keyboard and mouse never came to Frametop
// (Daenon, 2026-11-07).
bool g_windowApp = false;

// The shared clipboard (stream/clipboard.h, docs/shared-clipboard.md): with "clipboard": true
// on its host in the layout (Frametop Remote Displays: Share the clipboard), the desktop's text
// or image goes to the host when typing goes to this panel ("focus", or a key after a blur),
// and the host's comes back when typing leaves it ("blur"). The desktop's side is ft-clip, next
// to this program, on the Frametop desktop's own Wayland socket; the host's is
// Client::Clipboard. Both run on a thread of their own, in order, so the stream never waits for
// them. Only sizes are logged, never the content.
struct SharedClipboard {
    Client *client = nullptr;
    std::string host, state, pin, clip, wayland;
    std::atomic<bool> on{false}, gone{false};
    bool images = true;  // the thread's: false once the host says it takes text only
    bool focused = false;  // the main thread's
    int64_t checkAt = 0;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<char> ops;  // 's' send the desktop's clipboard, 'b' bring the host's back
    std::atomic<bool> quit{false};  // set under mu, read by a sync under way too
    std::set<std::string> said;  // the thread's: each problem is logged once
    std::thread thread;

    ~SharedClipboard() { Close(); }
    void Open(Client *c, const std::string &address) {
        client = c, host = address;
        const char *run = std::getenv("XDG_RUNTIME_DIR");
        state = clipboard::StateFile(run, hostinput::RelayId(address));
        if (state.empty()) {
            std::printf("clipboard: no XDG_RUNTIME_DIR, so it isn't shared\n");
            gone = true;
            return;
        }
        wayland = std::string(run) + "/frametop/wayland-0";  // session/frametop-session.sh
        std::ifstream p(KeyDir("hosts") + "/" + address + ".pin");
        if (!(p >> pin)) pin.clear();
        char exe[4096];
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        clip = n > 0 ? std::string(exe, size_t(n)) : "ft-stream";
        clip = clip.substr(0, clip.rfind('/') + 1) + "ft-clip";
        thread = std::thread(&SharedClipboard::Loop, this);
    }
    void Close() {
        if (!thread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mu);
            quit = true;
        }
        cv.notify_one();
        thread.join();  // a sync under way stops within 0.1 s (quit)
    }
    // The main loop, every pass: the switch, read again every 3 s.
    void Tick(int64_t now) {
        if (now < checkAt) return;
        checkAt = now + 3'000'000'000;
        const bool want = HostFlag(host, "clipboard");
        if (want != on.exchange(want)) std::printf("clipboard: %s with %s\n", want ? "shared" : "not shared", host.c_str());
    }
    void Focus() {
        if (focused) return;
        focused = true;
        Queue('s');
    }
    void Blur() {
        if (!focused) return;
        focused = false;
        Queue('b');
    }
    void Queue(char op) {
        if (!on || gone) return;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (ops.empty() || ops.back() != op) ops.push_back(op);
        }
        cv.notify_one();
    }
    void Loop() {
        for (;;) {
            char op;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [&] { return quit || !ops.empty(); });
                if (quit) return;
                op = ops.front();
                ops.pop_front();
            }
            if (op == 's') Send();
            else Bring();
            std::fflush(stdout);
        }
    }
    void Say(const std::string &why) {
        if (said.insert(why).second) std::printf("clipboard: %s\n", why.c_str());
    }
    bool Answered(long code, bool set, bool image) {
        if (quit) return false;  // the stream ends: nothing to say about a request it cut short
        std::string why;
        const clipboard::Answer a = clipboard::Classify(code, set, image, &why);
        if (a == clipboard::Answer::Ok) return true;
        Say(why);
        if (a == clipboard::Answer::Gone) gone = true;  // not again from this stream
        if (a == clipboard::Answer::TextOnly) images = false;  // nor images
        return false;
    }
    // The desktop's text, else its PNG image; empty when it holds neither. False when there's
    // no desktop to ask.
    bool Desktop(clipboard::Content *c) {
        *c = {};
        for (const bool image : {false, true}) {
            if (image && !images) break;
            const char *what = image ? "get-image" : "get";
            const int st = Run(what, nullptr, &c->data);
            if (st == 2 || st < 0) return Say(std::string("no Frametop desktop to share the clipboard with (ft-clip ") +
                                              what + ": " + std::to_string(st) + ")"), false;
            if (st == 0) return c->image = image, true;
            c->data.clear();
        }
        return true;
    }
    static const char *Kind(const clipboard::Content &c) { return c.image ? "image" : "text"; }
    void Send() {
        if (!on) return;  // turned off while it waited
        clipboard::Content c;
        if (!Desktop(&c) || !clipboard::ShouldPush(c, clipboard::ReadLast(state))) return;
        std::string body = c.data;
        if (!Answered(client->Clipboard(true, c.image, &body, pin, &quit), true, c.image)) return;
        clipboard::WriteLast(state, clipboard::Hash(c));
        std::printf("clipboard: sent %zu bytes of %s to %s\n", c.data.size(), Kind(c), host.c_str());
    }
    void Bring() {
        if (!on) return;
        // Its text, else (a host that shares images) its image.
        clipboard::Content h, desktop;
        if (!Answered(client->Clipboard(false, false, &h.data, pin, &quit), false, false)) return;
        if (h.data.empty() && images) {
            h.image = true;
            if (!Answered(client->Clipboard(false, true, &h.data, pin, &quit), false, true)) return;
        }
        if (!Desktop(&desktop)) return;
        if (!clipboard::ShouldPull(h, desktop, clipboard::ReadLast(state))) {
            if (clipboard::Fits(h)) clipboard::WriteLast(state, clipboard::Hash(h));  // the same
            return;
        }
        if (!h.image) h.data = clipboard::Normalize(h.data);
        const char *what = h.image ? "set-image" : "set";
        const int st = Run(what, &h.data, nullptr);
        if (st != 0) return Say(std::string("ft-clip ") + what + ": " + std::to_string(st));
        clipboard::WriteLast(state, clipboard::Hash(h));
        std::printf("clipboard: brought %zu bytes of %s from %s\n", h.data.size(), Kind(h), host.c_str());
    }
    // ft-clip WHAT on the desktop's socket, with *in on its stdin, its stdout in *out. Its exit
    // status, -1 if it didn't run or didn't finish in time (it's killed then).
    int Run(const char *what, const std::string *in, std::string *out) {
        const bool image = std::strstr(what, "image") != nullptr;
        const size_t max = image ? clipboard::kMaxImage : clipboard::kMaxText;
        int to[2] = {-1, -1}, from[2] = {-1, -1};
        if (pipe2(to, O_CLOEXEC) != 0) return -1;
        if (pipe2(from, O_CLOEXEC) != 0) return close(to[0]), close(to[1]), -1;
        std::vector<std::string> env{"WAYLAND_DISPLAY=" + wayland};
        for (char **e = environ; *e; ++e)
            if (std::strncmp(*e, "WAYLAND_DISPLAY=", 16) != 0) env.push_back(*e);
        std::vector<char *> envp;
        for (auto &e : env) envp.push_back(e.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, to[0], 0);
        if (out) posix_spawn_file_actions_adddup2(&fa, from[1], 1);
        else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        char *argv[] = {clip.data(), const_cast<char *>(what), nullptr};
        pid_t pid = -1;
        const int r = posix_spawn(&pid, clip.c_str(), &fa, nullptr, argv, envp.data());
        posix_spawn_file_actions_destroy(&fa);
        close(to[0]), close(from[1]);
        if (r != 0) return close(to[1]), close(from[0]), Say("can't run " + clip + ": " + std::strerror(r)), -1;
        // ft-clip reads all of its input before it does anything, so this can't wait on it.
        for (size_t at = 0; in && at < in->size();) {
            const ssize_t w = write(to[1], in->data() + at, in->size() - at);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            at += size_t(w);
        }
        close(to[1]);
        const int64_t until = MonoNs() + (image ? 12'000'000'000 : 3'000'000'000);  // its own wait: 10 s, 2 s
        bool late = false, big = false;
        if (out) {
            char buf[65536];
            for (;;) {
                const int64_t left = (until - MonoNs()) / 1'000'000;
                if (left <= 0 || quit) {
                    late = true;
                    break;
                }
                pollfd p{from[0], POLLIN, 0};
                const int polled = poll(&p, 1, int(std::min<int64_t>(left, 100)));  // quit within 0.1 s
                if (polled == 0 || (polled < 0 && errno == EINTR)) continue;
                if (polled < 0) {
                    late = true;
                    break;
                }
                const ssize_t n = read(from[0], buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                if (out->size() + size_t(n) > max) {  // ft-clip says no first; never more
                    big = true;
                    break;
                }
                out->append(buf, size_t(n));
            }
        }
        close(from[0]);
        int st = 0;
        while (!late && !big) {
            const pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (w < 0 || MonoNs() >= until || quit) break;
            usleep(10'000);
        }
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        if (!quit) Say(std::string("ft-clip ") + what + (big ? " gave more than it may" : " didn't finish in time"));
        return -1;
    }
};
SharedClipboard g_clip;

void HostInputRecord(const unsigned char *data, int length) { g_hostin.Got(data, length); }

int Submit(PDECODE_UNIT du) {
    const bool idr = du->frameType == FRAME_TYPE_IDR;
    ++g_received, g_bytes += uint64_t(du->fullLength);
    if (idr) g_waitIdr = false;
    if (!idr && g_waitIdr) return ++g_skipped, DR_OK;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_units.size() >= 16) {  // the main loop fell far behind: start over from an IDR
        g_units.clear();
        g_waitIdr = true;
        return DR_NEED_IDR;
    }
    Unit u;
    u.data.resize(size_t(du->fullLength));
    size_t at = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) std::memcpy(&u.data[at], e->data, size_t(e->length)), at += size_t(e->length);
    g_units.push_back(std::move(u));
    const uint64_t one = 1;
    if (write(g_wake, &one, sizeof one) < 0) {}
    return DR_OK;
}

int Setup(int format, int width, int height, int fps, void *, int) {
    std::printf("stream: %s %dx%d at %d fps\n", (format & VIDEO_FORMAT_MASK_H265) ? "HEVC" : "H.264", width, height, fps);
    return (format & VIDEO_FORMAT_MASK_H265) ? 0 : -1;
}

void Log(const char *format, ...) {
    va_list a;
    va_start(a, format);
    std::printf("moonlight: ");
    std::vprintf(format, a);
    va_end(a);
    std::fflush(stdout);
}

void StageFailed(int stage, int error) { std::printf("stream: %s failed (%d)\n", LiGetStageName(stage), error); }

void Terminated(int error) {
    std::printf("stream: connection ended (%d)\n", error);
    g_ended = error + 1;
    g_stop = 1;
}

// The socket to ft-screens.
struct Viewer {
    int fd = -1;

    bool Send(const std::string &msg, const int *fds = nullptr, int nfds = 0) const {
        iovec iov = {const_cast<char *>(msg.data()), msg.size()};
        msghdr m{};
        m.msg_iov = &iov, m.msg_iovlen = 1;
        char control[CMSG_SPACE(sizeof(int) * 4)] = {};
        if (nfds) {
            m.msg_control = control, m.msg_controllen = CMSG_SPACE(sizeof(int) * size_t(nfds));
            cmsghdr *c = CMSG_FIRSTHDR(&m);
            c->cmsg_level = SOL_SOCKET, c->cmsg_type = SCM_RIGHTS, c->cmsg_len = CMSG_LEN(sizeof(int) * size_t(nfds));
            std::memcpy(CMSG_DATA(c), fds, sizeof(int) * size_t(nfds));
        }
        return sendmsg(fd, &m, MSG_NOSIGNAL) == ssize_t(msg.size());
    }

    // One message, or "" when there's none now; false when ft-screens is gone.
    bool Read(std::string *msg, bool wait) const {
        char buf[512];
        const ssize_t n = recv(fd, buf, sizeof buf - 1, wait ? 0 : MSG_DONTWAIT);
        if (n > 0) return *msg = std::string(buf, size_t(n)), true;
        msg->clear();
        return n < 0 && (errno == EAGAIN || errno == EINTR);
    }
};

// Typing: Moonlight wants Windows virtual-key codes and the modifiers held.
struct Keys {
    char modifiers = 0;
    std::set<unsigned> held;
    // Meta held back (a Win press waits for the next key) and Metas whose combination was Frametop's:
    // the input relay passes Meta's press on at once, and when it then makes a key combination of
    // Frametop's own (Meta+A: carry_toggle), it keeps the key and sends F24 before Meta's release, so
    // the desktop doesn't take it for a Meta tap. Here F24 after a held-back Meta drops the three (user,
    // 2026-11-01: "the key combo is reaching vibepollo remote desktops": dropped F24 left a Win tap,
    // which opened Start; sent as VK_F24, Win+F24 darkened the host's desktop). Any other key sends
    // Meta first; Meta's release alone sends the tap.
    std::set<unsigned> metaWaiting, metaTaken;

    // F13 to F23 too, past the table's end (VK_F13..VK_F23).
    static unsigned Vk(unsigned code) {
        if (code >= KEY_F13 && code <= KEY_F23) return 0x7C + (code - KEY_F13);
        return code < sizeof keyCodes / sizeof keyCodes[0] ? keyCodes[code] : 0;
    }

    void Send(unsigned code, bool down) {
        const bool meta = code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
        if (meta && down) return void(metaWaiting.insert(code));
        if (meta && !down) {
            if (metaTaken.erase(code)) return;
            if (metaWaiting.erase(code)) Put(code, true);   // (alone: a tap)
            return Put(code, false);
        }
        if (code == KEY_F24 && (!metaWaiting.empty() || !metaTaken.empty())) {
            metaTaken.insert(metaWaiting.begin(), metaWaiting.end());
            metaWaiting.clear();
            return;
        }
        if (down)
            for (const unsigned m : metaWaiting) Put(m, true);
        if (down) metaWaiting.clear();
        Put(code, down);
    }

    void Put(unsigned code, bool down) {
        const unsigned vk = Vk(code);
        if (!vk) return;
        if (down) held.insert(code);
        else if (!held.erase(code)) return;  // never went down here
        char m = 0;
        switch (code) {
            case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: m = MODIFIER_SHIFT; break;
            case KEY_LEFTALT: case KEY_RIGHTALT: m = MODIFIER_ALT; break;
            case KEY_LEFTCTRL: case KEY_RIGHTCTRL: m = MODIFIER_CTRL; break;
            case KEY_LEFTMETA: case KEY_RIGHTMETA: m = MODIFIER_META; break;
        }
        if (m) modifiers = down ? char(modifiers | m) : char(modifiers & ~m);
        LiSendKeyboardEvent(short(0x80 << 8 | vk), down ? KEY_ACTION_DOWN : KEY_ACTION_UP, modifiers);
    }

    void Blur() {
        metaWaiting.clear(), metaTaken.clear();
        while (!held.empty()) Put(*held.begin(), false);
    }
};

// Gamepad mode's pad ("pad ..." from ft-screens): controller 0, an Xbox pad with analog
// triggers, there from its first state till "pad off" or a blur. The buttons are XInput's,
// which are Moonlight's (A_FLAG and the rest); the guide button isn't sent.
struct Pad {
    bool here = false;

    void Message(const std::string &msg) {
        if (msg == "pad off") return Off();
        unsigned b;
        int lt, rt, lx, ly, rx, ry;
        if (std::sscanf(msg.c_str(), "pad %x %d %d %d %d %d %d", &b, &lt, &rt, &lx, &ly, &rx, &ry) != 7) return;
        constexpr unsigned kButtons = A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG |
                                      LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG;
        if (!here) {
            here = true;
            LiSendControllerArrivalEvent(0, 1, LI_CTYPE_XBOX, kButtons, LI_CCAP_ANALOG_TRIGGERS);
            std::printf("gamepad: on\n");
            std::fflush(stdout);
        }
        const auto axis = [](int v) { return short(std::clamp(v, -32767, 32767)); };
        LiSendMultiControllerEvent(0, 1, int(b & kButtons), (unsigned char)std::clamp(lt, 0, 255),
                                   (unsigned char)std::clamp(rt, 0, 255), axis(lx), axis(ly), axis(rx), axis(ry));
    }

    void Off() {
        if (!here) return;
        here = false;
        LiSendMultiControllerEvent(0, 1, 0, 0, 0, 0, 0, 0, 0);  // nothing held as it goes
        LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0);
        std::printf("gamepad: off\n");
        std::fflush(stdout);
    }
};

int MoonlightButton(unsigned code) {
    switch (code) {
        case BTN_RIGHT: return BUTTON_RIGHT;
        case BTN_MIDDLE: return BUTTON_MIDDLE;
        case BTN_SIDE: return BUTTON_X1;
        case BTN_EXTRA: return BUTTON_X2;
        default: return BUTTON_LEFT;
    }
}

// Whether a host's GameStream port (47989, or the instance's) takes a connection within `ms`.
bool Answers(const std::string &address, int port, int ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(address.c_str(), std::to_string(port ? port : kGameStreamPort).c_str(), &hints, &res) != 0)
        return false;
    bool ok = false;
    for (addrinfo *a = res; a && !ok; a = a->ai_next) {
        const int s = socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (s < 0) continue;
        if (connect(s, a->ai_addr, a->ai_addrlen) == 0) ok = true;
        else if (errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            ok = poll(&p, 1, ms) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

// The address to reach a host at, by its "route" in ~/.config/frametop-layout.json
// (Frametop Remote Displays sets it): "network" is its own address; "dongle" one of its
// "direct" addresses, such as its Steam Link dongle on the Frame's own hotspot, and none
// (empty) while none answers; "auto" (the default) the first direct one that answers, else
// its own. Through the home router a stream lost a frame's packets in bursts (0.33% of
// packets, 1.8% of frames at 50 Mbit/s); over the dongle, none (2026-10-07).
nlohmann::json Layout() {
    std::ifstream f(std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/frametop-layout.json");
    const nlohmann::json layout = nlohmann::json::parse(f, nullptr, false);
    return layout.is_object() && layout.contains("hosts") && layout["hosts"].is_array() ? layout : nlohmann::json();
}

std::string Route(const std::string &host, int port) {
    const nlohmann::json layout = Layout();
    if (layout.is_null()) return host;
    for (const auto &h : layout["hosts"]) {
        if (!h.is_object() || h.value("address", "") != host) continue;
        const std::string route = h.value("route", "auto");
        if (route == "network") return host;
        if (h.contains("direct") && h["direct"].is_array())
            for (const auto &d : h["direct"])
                // A second: right after Vibepollo starts, 300 ms sent streams the long way.
                if (d.is_string() && Answers(d.get<std::string>(), port, 1000)) return d.get<std::string>();
        return route == "dongle" ? "" : host;
    }
    return host;
}

// Whether the host's entry in the layout has `key` true: "mic" (Frametop Remote Displays' Send my
// microphone) or "input" (Use this computer's keyboard and mouse). Read again every few seconds
// while streaming.
bool HostFlag(const std::string &host, const char *key) {
    const nlohmann::json layout = Layout();
    if (layout.is_null()) return false;
    for (const auto &h : layout["hosts"])
        if (h.is_object() && h.value("address", "") == host) return h.contains(key) && h[key].is_boolean() && h[key].get<bool>();
    return false;
}

// The port of the display whose client is `id`, from the layout, or 0. A Mac runs a Sunshine
// instance per display, each on its own port, and a client is paired with one of them only. An
// ft-layout from before ports (an older release's Reset Screen Layout or profile launchers) asks
// for HOST alone: that reached the instance on 47989, where the client isn't paired, so the
// display froze and its pointer went to the main display (2026-10-10).
int LayoutPort(const std::string &host, const std::string &id) {
    const nlohmann::json layout = Layout();
    if (layout.is_null()) return 0;
    for (const auto &h : layout["hosts"]) {
        if (!h.is_object() || h.value("address", "") != host || !h.contains("displays") || !h["displays"].is_array())
            continue;
        for (const auto &d : h["displays"])
            if (d.is_object() && d.value("client", "") == id && d.contains("port") && d["port"].is_number_integer())
                return d["port"].get<int>();
    }
    return 0;
}

// --app window:ADDRESS: the host puts the window on an output of its own, width x height.
// The HTTP status (400 when the window isn't there), with the output's name or the host's why.
long OpenWindow(WebApi &api, const std::string &address, int width, int height, std::string *output,
                std::string *why) {
    const nlohmann::json body = {{"address", address}, {"width", width}, {"height", height}};
    nlohmann::json r;
    const long code = api.Call("POST", "/api/frametop/windows/open", &body, &r);
    if (code == 200 && r.is_object() && r.contains("output") && r["output"].is_string()) {
        *output = r["output"].get<std::string>();
        return code;
    }
    *why = r.is_object() ? r.value("error", "") : "";
    return code == 200 ? -1 : code;
}

int Stream(Client &client, const std::string &app, int fd, int width, int height, int fps, int bitrate) {
    Viewer viewer{fd};
    // ft-screens' event loop blocks SIGTERM (it takes it through a signalfd), and an older
    // ft-screens passed that on: SIGTERM, and so the death signal, never came.
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    prctl(PR_SET_PDEATHSIG, SIGTERM);  // ft-screens went: so does this
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    signal(SIGPIPE, SIG_IGN);
    if (!client.server.paired) {
        viewer.Send("state lost not paired");
        return std::fprintf(stderr, "not paired as %s: run ft-stream pair %s --id %s\n", client.id.c_str(),
                            client.host.c_str(), client.id.c_str()), 1;
    }
    const int appId = client.FindApp(app);
    if (!appId) return viewer.Send("state lost no such display"), 1;

    // The first word from ft-screens: what SteamVR imports.
    Converter conv;
    conv.toSteamVR = false;
    for (std::string msg; viewer.Read(&msg, true) && !msg.empty();) {
        if (msg.rfind("modifiers", 0) != 0) continue;
        std::istringstream words(msg.substr(9));
        for (std::string m; words >> m;) conv.modifiers.push_back(std::strtoull(m.c_str(), nullptr, 16));
        break;
    }
    Decoder dec;
    dec.toSteamVR = false;
    if (!dec.Open("/dev/video-dec0", 6) || !conv.Init()) return viewer.Send("state lost no decoder"), 1;

    STREAM_CONFIGURATION cfg;
    LiInitializeStreamConfiguration(&cfg);
    cfg.width = width, cfg.height = height, cfg.fps = fps, cfg.bitrate = bitrate;
    cfg.packetSize = 1392;
    cfg.streamingRemotely = STREAM_CFG_AUTO;
    cfg.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg.supportedVideoFormats = VIDEO_FORMAT_H265;
    cfg.clientRefreshRateX100 = fps * 100;
    cfg.colorSpace = COLORSPACE_REC_709;
    cfg.colorRange = COLOR_RANGE_LIMITED;
    cfg.encryptionFlags = ENCFLG_AUDIO;

    viewer.Send("state connecting");
    // Remote Monitors take a moment to appear; Vibepollo says 503 until then.
    const bool resume = appId != kRemoteMonitor && appId != kFrametopDisplay && client.server.currentGame == appId;
    std::string message;
    int code = 0;
    for (int tries = 0; tries < 60 && !g_stop; ++tries) {
        code = client.Launch(cfg, appId, resume, &message);
        if (code != 503) break;
        usleep(500'000);
    }
    std::printf("%s app %d: %d %s\n", resume ? "resume" : "launch", appId, code, message.c_str());
    if (code != 200) return viewer.Send("state lost " + std::to_string(code) + " " + message), 1;

    g_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    DECODER_RENDERER_CALLBACKS dr;
    LiInitializeVideoCallbacks(&dr);
    dr.setup = Setup;
    dr.submitDecodeUnit = Submit;
    CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeConnectionCallbacks(&cl);
    cl.logMessage = Log;
    cl.stageFailed = StageFailed;
    cl.connectionTerminated = Terminated;
    AUDIO_RENDERER_CALLBACKS ar;
    LiInitializeAudioCallbacks(&ar);
    ar.init = AudioInit;
    ar.cleanup = AudioCleanup;
    ar.decodeAndPlaySample = AudioPlay;
    ar.capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
    g_audio.name = "Remote display: " + g_host;
    g_audio.Claim(g_host, MonoNs());
    g_mic.name = "Remote display microphone: " + g_host;
    if (!g_windowApp) g_hostin.Open(g_host);
    g_clip.Open(&client, g_host);
    LiSetFrametopHostInputCallback(HostInputRecord);
    if (LiStartConnection(&client.server.serverInfo, &cfg, &cl, &dr, &ar, nullptr, 0, nullptr, 0) != 0)
        return viewer.Send("state lost can't connect"), g_clip.Close(), 1;
    std::printf("stream: connected, %d kbit/s asked\n", bitrate);
    std::fflush(stdout);

    Keys keys;
    Pad pad;
    bool owned[3] = {};  // ring buffers ft-screens shows or is about to
    bool live = false;
    uint64_t seq = 0;
    int64_t lastReport = MonoNs(), cpu0 = CpuNs(), lastShown = 0, allHeldSince = -1;
    int64_t micCheck = 0;
    int shown = 0, dropped = 0, rested = 0, lastSent = -1;
    while (!g_stop) {
        pollfd p[3] = {{dec.fd, POLLIN | POLLPRI | POLLOUT, 0}, {g_wake, POLLIN, 0}, {viewer.fd, POLLIN, 0}};
        if (poll(p, 3, 50) > 0 && (p[0].revents & POLLERR)) usleep(1000);
        uint64_t n;
        if (read(g_wake, &n, sizeof n) < 0) {}
        // ft-screens: input, released buffers, attention.
        std::string msg;
        bool open = true;
        while ((open = viewer.Read(&msg, false)) && !msg.empty()) {
            double x, y;
            unsigned c;
            int down, i;
            if (std::sscanf(msg.c_str(), "move %lf %lf", &x, &y) == 2) {
                LiSendMousePositionEvent(short(std::clamp(int(x), 0, width - 1)), short(std::clamp(int(y), 0, height - 1)),
                                         short(width), short(height));
            } else if (std::sscanf(msg.c_str(), "button %u %d", &c, &down) == 2) {
                LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, MoonlightButton(c));
            } else if (std::sscanf(msg.c_str(), "scroll %lf %lf", &x, &y) == 2) {
                if (y != 0) LiSendHighResScrollEvent(short(-y * 120));
                if (x != 0) LiSendHighResHScrollEvent(short(x * 120));
            } else if (std::sscanf(msg.c_str(), "key %u %d", &c, &down) == 2) {
                if (down) g_clip.Focus();  // typing came back without a click (an older ft-screens says no "focus")
                keys.Send(c, down != 0);
            } else if (msg == "focus") {
                g_clip.Focus();
            } else if (msg == "blur") {
                keys.Blur();
                pad.Off();
                g_clip.Blur();
            } else if (msg.rfind("pad ", 0) == 0) {
                pad.Message(msg);
            } else if (std::sscanf(msg.c_str(), "release %d", &i) == 1) {
                if (i >= 0 && i < 3) owned[i] = false;
            } else if (msg.rfind("attention ", 0) == 0) {
                g_hidden = msg == "attention hidden";
            } else if (msg == "quit") {
                g_stop = 1;
            }
        }
        // ft-screens closed its end. (Asking again here once lost whatever came in between:
        // a "release" lost so kept a buffer from the ring for good, and after three the
        // panel froze, 2026-10-07.)
        if (!open) break;
        g_audio.Claim(g_host, MonoNs());
        g_hostin.Tick(MonoNs(), live);
        g_clip.Tick(MonoNs());
        if (MonoNs() >= micCheck) micCheck = MonoNs() + 3'000'000'000, g_mic.Want(g_audio.on && HostFlag(g_host, "mic"));
        if (dec.Events()) {
            if (!conv.Setup(dec)) break;
            const int fds[3] = {conv.out[0].fd, conv.out[1].fd, conv.out[2].fd};
            char b[160];
            std::snprintf(b, sizeof b, "buffers %u %u %x %llx %u %u", conv.width, conv.height, DRM_FORMAT_ABGR8888,
                          (unsigned long long)conv.out[0].modifier, conv.out[0].offset, conv.out[0].stride);
            if (!viewer.Send(b, fds, 3)) break;
            for (bool &o : owned) o = false;
        }
        dec.Reclaim();
        for (;;) {
            Unit u;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                if (g_units.empty()) break;
                if (!dec.Feed(g_units.front().data.data(), g_units.front().data.size(), seq)) break;
                g_units.pop_front();
            }
            ++seq;
        }
        uint64_t frame = 0;
        bool last = false;
        int i;
        while ((i = dec.Decoded(&frame, &last)) >= 0) {
            const int64_t at0 = MonoNs();
            if (g_hidden && at0 - lastShown < 1'000'000'000 / kHiddenFps) {  // out of sight
                dec.Requeue(i);
                ++rested;
                continue;
            }
            int at = -1;
            for (int k = 0; k < 3 && at < 0; ++k)
                if (!owned[(conv.next + k) % 3]) at = (conv.next + k) % 3;
            if (at < 0) {  // ft-screens still has all three: this picture is skipped
                dec.Requeue(i);
                ++dropped;
                // ft-screens hands one back with every new picture it shows. If none comes
                // back for this long, they were lost: all but the one it shows are ours again.
                if (allHeldSince < 0) allHeldSince = at0;
                else if (at0 - allHeldSince > 2'000'000'000) {
                    std::printf("ring: no buffer back from ft-screens for 2 s, taking them back\n");
                    for (int k = 0; k < 3; ++k) owned[k] = k == lastSent;
                    allHeldSince = -1;
                }
                continue;
            }
            allHeldSince = -1;
            conv.ConvertInto(i, at);
            conv.next = (at + 1) % 3;
            dec.Requeue(i);
            owned[at] = true;
            if (!viewer.Send("frame " + std::to_string(at))) g_stop = 1;
            lastSent = at, lastShown = at0;
            ++shown;
            if (!live) live = true, viewer.Send(client.host != g_host ? "state live via dongle" : "state live via network");
        }
        const int64_t now = MonoNs();
        if (now - lastReport >= 10'000'000'000) {
            const double secs = (now - lastReport) / 1e9;
            uint32_t rtt = 0, rttVar = 0;
            LiGetEstimatedRttInfo(&rtt, &rttVar);
            const int micSent = g_mic.sent.exchange(0), hostEvents = g_hostin.TakeEvents();
            std::printf("%s: in %.1f fps (%d skipped), shown %.1f fps (%d dropped, %d out of sight), %.1f Mbit/s, rtt %u ms, "
                        "convert %.1f ms, CPU %.1f%% of a core%s%s%s\n",
                        g_hidden ? "hidden" : "shown", g_received.exchange(0) / secs, g_skipped.exchange(0), shown / secs,
                        dropped, rested, g_bytes.exchange(0) * 8 / secs / 1e6, rtt, shown ? conv.msSum / shown : 0.0,
                        100 * (CpuNs() - cpu0) / 1e9 / secs, g_audio.on ? ", playing its sound" : "",
                        micSent ? (", mic " + std::to_string(int(micSent / secs + 0.5)) + " packets/s").c_str() : "",
                        hostEvents ? (", host input " + std::to_string(int(hostEvents / secs + 0.5)) + " reports/s").c_str() : "");
            std::fflush(stdout);
            conv.msSum = 0, shown = dropped = rested = 0, lastReport = now, cpu0 = CpuNs();
        }
    }
    const int ended = g_ended;
    g_stop = 1;
    // The way out can hang (LiStopConnection, the decoder's close, the HTTP call), and a hung
    // ft-stream keeps its host's input and audio locks. ft-screens kills one 20 s after it asked
    // it to end or heard "lost" (screens/remote.c), but not once it's gone itself: a desktop stop
    // leaves its ft-streams behind. So this one ends itself 25 s from here (SIGALRM's default
    // action) whoever is left.
    alarm(kExitWatchdogS);
    // "lost" before LiStopConnection, not after: a connection the host ended that hangs in it
    // used to stay "live" in ft-screens, which never killed it.
    viewer.Send(ended ? "state lost the host ended the stream" : "state lost");
    g_mic.Stop();  // no packet after this, before the connection goes
    g_hostin.Stop(hostinput::Want::kDrop);  // the host's keyboard and mouse go back now, not a lease later
    if (!ended) pad.Off();  // its buttons come up on the host (a lost connection drops it anyway)
    LiStopConnection();
    dec.Close();
    if (appId == kRemoteMonitor) {
        code = client.Launch(cfg, kDisconnectMonitor, false, &message);
        std::printf("release monitor: %d %s\n", code, message.c_str());
    }
    g_clip.Close();  // a sync under way ends within seconds (the host says no now)
    return ended > 1 ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "usage: ft-stream pair|grant|unpair|monitors|release|stream HOST[:PORT] --id NAME [options]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    std::string host = argv[2];
    int port = 0;
    if (!SplitPort(&host, &port)) return std::fprintf(stderr, "bad port in %s\n", argv[2]), 2;
    std::string id, app = "primary", tokenFile;
    int width = 2560, height = 1440, fps = 60, bitrate = 0, fd = -1;
    bool login = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--id") id = v, ++i;
        else if (a == "--token") tokenFile = v, ++i;
        else if (a == "--login") login = true;
        else if (a == "--app") app = v, ++i;
        else if (a == "--size") std::sscanf(v, "%dx%d", &width, &height), ++i;
        else if (a == "--fps") fps = std::atoi(v), ++i;
        else if (a == "--bitrate") bitrate = std::atoi(v), ++i;
        else if (a == "--fd") fd = std::atoi(v), ++i;
        else return std::fprintf(stderr, "unknown option %s\n", argv[i]), 2;
    }
    if (!port && !id.empty() && (port = LayoutPort(host, id)))
        std::printf("%s: %s's port is %d (the layout)\n", host.c_str(), id.c_str(), port);
    const std::string shared = TokenFile(host);
    if (tokenFile.empty()) tokenFile = TokenFile(host, port);
    WebApi api;
    const bool haveToken =
        login ? api.LoadLogin(host, port, stdin) : api.Load(host, tokenFile, tokenFile == shared ? 0 : port);
    if (login && !haveToken) return std::fprintf(stderr, "--login: no user name and password on stdin\n"), 2;
    if (cmd == "monitors") {
        if (!haveToken) return std::fprintf(stderr, "no token in %s\n", tokenFile.c_str()), 1;
        nlohmann::json r;
        const long code = api.Call("GET", "/api/display-devices?detail=full", nullptr, &r);
        if (code != 200) return std::fprintf(stderr, "display-devices: %ld\n", code), 1;
        std::printf("%s\n", r.dump(2).c_str());
        return 0;
    }
    if (id.empty()) return std::fprintf(stderr, "--id NAME is required: each display is its own client\n"), 2;
    if (width < 64 || height < 64 || width > 8192 || height > 8192 || fps < 1 || fps > 240)
        return std::fprintf(stderr, "bad --size or --fps\n"), 2;
    // Moonlight's rule of thumb, about 20 Mbit/s for 1080p60, scaled by pixels and fps.
    if (!bitrate) bitrate = int(std::min(150000.0, 20000.0 * width * height * fps / (1920.0 * 1080 * 60)));

    g_host = host;
    const std::string route = cmd == "stream" ? Route(host, port) : host;
    if (route.empty()) {  // dongle only, and it isn't up: ft-screens tries again later
        std::fprintf(stderr, "%s: no direct address answers (route dongle)\n", host.c_str());
        if (fd >= 0) Viewer{fd}.Send("state lost the dongle link is down");
        return 1;
    }
    Client client;
    if (!client.Connect(route, id, port)) return 1;
    std::printf("host %s%s%s: %s, %s, current app %d\n", host.c_str(), port ? (":" + std::to_string(port)).c_str() : "",
                route != host ? (" through " + route).c_str() : "",
                client.server.serverInfo.serverInfoAppVersion, client.server.paired ? "paired" : "not paired",
                client.server.currentGame);
    if (cmd == "pair") {
        if (client.server.paired) return std::printf("already paired as %s\n", id.c_str()), 0;
        return client.Pair(haveToken ? &api : nullptr) ? 0 : 1;
    }
    if (cmd == "grant") {
        if (!haveToken || login) return std::fprintf(stderr, "no token in %s\n", tokenFile.c_str()), 1;
        return client.GrantByName(api) ? 0 : 1;
    }
    if (cmd == "unpair") return client.Unpair() ? 0 : 1;
    if (cmd == "release") {
        STREAM_CONFIGURATION cfg;
        LiInitializeStreamConfiguration(&cfg);
        cfg.width = width, cfg.height = height, cfg.fps = fps;
        std::string message;
        const int code = client.Launch(cfg, kDisconnectMonitor, false, &message);
        std::printf("release: %d %s\n", code, message.c_str());
        return code == 200 || code == 410 ? 0 : 1;
    }
    if (cmd == "stream") {
        if (fd < 0) return std::fprintf(stderr, "stream needs --fd: ft-screens starts it\n"), 2;
        if (app.rfind("window:", 0) == 0) {
            if (!haveToken || login) return std::fprintf(stderr, "no token in %s\n", tokenFile.c_str()), 1;
            // The host's Web UI where the stream goes too (the dongle's address on route dongle).
            if (route != host) api.base = "https://" + route + api.base.substr(api.base.rfind(':'));
            std::string output, why;
            const long code = OpenWindow(api, app.substr(7), width, height, &output, &why);
            if (code != 200) {
                std::fprintf(stderr, "window %s: %ld %s\n", app.c_str() + 7, code, why.c_str());
                // Only a window that's gone waits as long as a missing app does ("lost 404"):
                // a full host or a Hyprland hiccup is worth the usual retry.
                const bool gone = code == 400 && why.rfind("no window", 0) == 0;
                Viewer{fd}.Send("state lost " + std::to_string(gone ? 404 : code) + " " + why);
                return 1;
            }
            std::printf("window %s is on %s\n", app.c_str() + 7, output.c_str());
            app = "display:" + output;
            g_windowApp = true;
        }
        return Stream(client, app, fd, width, height, fps, bitrate);
    }
    return std::fprintf(stderr, "unknown command %s\n", cmd.c_str()), 2;
}
