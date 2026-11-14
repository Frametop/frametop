// A voice's way from the link to the ears (together/voice): the order its packets are played
// in (jitter.h), how loud someone is in each ear (spatial.h), Opus there and back (codec.h)
// with what a peer might send instead, the room's voices mixed (voices.h), and the wearer's
// mouth as their own voice moves it (mouthsay.h).
#include "../proto/voicewire.h"
#include "../voice/codec.h"
#include "../voice/jitter.h"
#include "../voice/mouthsay.h"
#include "../voice/spatial.h"
#include "../voice/voices.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using voice::Jitter;
using voice::kFrame;

static int failed = 0;
static void check(const char *what, bool ok) {
    failed += !ok;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// What the next pulls give, a letter each: . nothing, the packet's first byte as a digit,
// r made up from the next, f filled in.
static std::string pulls(Jitter &j, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        const Jitter::Due d = j.Pull();
        out += d.what == Jitter::kNothing ? '.' : d.what == Jitter::kPacket ? char('0' + d.opus[0] % 10) : d.what == Jitter::kFromNext ? 'r' : 'f';
    }
    return out;
}
static void push(Jitter &j, int count) {
    const uint8_t opus[2] = {uint8_t(count % 10), 9};
    j.Push(uint16_t(count), opus, sizeof opus);
}

static void jitter() {
    Jitter j;
    check("with no voice there is nothing to play", pulls(j, 3) == "..." && !j.playing());
    push(j, 100);
    check("a voice starts three packets after its first one came", pulls(j, 3) == "..." && j.playing());
    push(j, 101), push(j, 102), push(j, 103);
    check("then each is played in its turn", pulls(j, 4) == "0123" && j.played() == 4);
    push(j, 105);   // 104 is lost
    check("a lost one is made up from the one after it, which is then played itself", pulls(j, 2) == "r5" && j.lost() == 1);
    push(j, 108);   // 106 and 107 are lost
    check("two lost in a row: the first is filled in, the second made up from the next", pulls(j, 3) == "fr8" && j.lost() == 3);
    check("when the voice stops, five are filled in, then there is silence and it has stopped; its end isn't a loss", pulls(j, 7) == "fffff.." && !j.playing() && j.lost() == 3);

    push(j, 300);
    check("the next sentence starts as the first did: three packets behind", pulls(j, 3) == "...");
    push(j, 301), push(j, 302);   // 301 comes 40 ms after it was due to come, with 302
    check("...so packets that come up to 60 ms late, in a bunch, are still in time", pulls(j, 3) == "012" && j.late() == 0);

    // we play faster than the sender sends: its turn comes before it does
    Jitter fast;
    push(fast, 10);
    pulls(fast, 4);   // 10 played
    check("a packet whose turn passed while nothing waited starts the voice again from it", (pulls(fast, 1) == "f" && (push(fast, 11), true)) && pulls(fast, 4) == "...1" &&
                                                                                                  fast.late() == 0);
    push(fast, 13), push(fast, 12);
    check("...but one that comes after a newer one that waits is dropped", fast.late() == 0 && (pulls(fast, 1), push(fast, 12), fast.late() == 1));

    // nobody plays for a while (the loudspeakers were closed), and packets keep coming
    Jitter stalled;
    for (int i = 0; i < 500; ++i) push(stalled, 1000 + i);
    check("packets that nobody plays don't pile up", stalled.waiting() <= size_t(Jitter::kMost) + 1 && stalled.skipped() > 400);
    const std::string resumed = pulls(stalled, 20);
    check("...and play goes on with the newest ones", resumed.substr(0, 13) == "...0123456789" && resumed.find_first_not_of("f.", 13) == std::string::npos);

    Jitter round;
    for (int i = 65530; i < 65546; ++i) push(round, i), pulls(round, 1);
    check("the count goes round without a break", round.played() == 13 && round.lost() == 0 && round.late() == 0);
    Jitter gap;
    push(gap, 5), push(gap, 6);
    pulls(gap, 5);
    push(gap, 12);   // the sender said nothing for 100 ms
    check("a pause between two words is that long here too", pulls(gap, 6) == "ffffr2");
}

static void spatial() {
    using voice::Ears;
    using voice::Hear;
    const double me[3] = {1, 1.6, 2};
    const auto at = [&](double x, double y, double z, double yaw = 0) {
        const double mouth[3] = {me[0] + x, me[1] + y, me[2] + z};
        return Hear(me, yaw, mouth);
    };
    const auto power = [](const Ears &e) { return double(e.left) * e.left + double(e.right) * e.right; };
    Ears e = at(0, 0, -1);
    check("someone a metre in front is in the middle, at full loudness", std::fabs(e.left - e.right) < 1e-6 && std::fabs(power(e) - 1) < 1e-6);
    e = at(1, 0, 0);
    const Ears l = at(-1, 0, 0);
    check("someone to the right is louder in the right ear, and the left keeps some", e.right > 2 * e.left && e.left > 0.1f);
    check("...and to the left it is the other way round, as loud", std::fabs(l.left - e.right) < 1e-6 && std::fabs(l.right - e.left) < 1e-6 && std::fabs(power(e) - 1) < 1e-6);
    check("further away is quieter, but never silent", power(at(0, 0, -2)) < 0.3 && power(at(0, 0, -4)) < power(at(0, 0, -2)) && power(at(0, 0, -90)) > 0.03);
    check("nearer than a metre is no louder", std::fabs(power(at(0, 0, -0.3)) - 1) < 1e-6);
    check("behind is a little quieter than in front, and still in the middle", power(at(0, 0, 1)) < power(at(0, 0, -1)) && power(at(0, 0, 1)) > 0.5 &&
                                                                              std::fabs(at(0, 0, 1).left - at(0, 0, 1).right) < 1e-6);
    check("straight above is the middle", std::fabs(at(0, 1, 0).left - at(0, 1, 0).right) < 1e-6);
    // facing +x (yaw -90 degrees: 0 faces -z, and a turn to the left counts up)
    e = at(0, 0, 1, -M_PI / 2);
    check("the head's turn counts: facing +x, someone at +z is to the right", e.right > 2 * e.left);
    e = at(1, 0, 0, -M_PI / 2);
    check("...and someone at +x is ahead", std::fabs(e.left - e.right) < 1e-6);
    e = at(0, 0, 0);
    check("someone where the listener is, is in the middle", e.left == voice::kMiddle.left && e.right == voice::kMiddle.right);
}

// 20 ms of something a voice codec takes for a voice: a hum at `hz` that swells five times a
// second, as syllables do (a steady note counts as noise, and isn't sent for long). Counted on
// from `from`.
static std::vector<float> hum(int from, double hz = 220, double loud = 0.3) {
    std::vector<float> pcm(kFrame);
    for (int i = 0; i < kFrame; ++i) {
        const double t = double(from * kFrame + i) / voice::kRate;
        pcm[size_t(i)] = float(loud * std::sin(2 * M_PI * hz * t) * (0.6 + 0.4 * std::sin(2 * M_PI * 5 * t)));
    }
    return pcm;
}
static double rms(const float *pcm, int n, int step = 1) {
    double squares = 0;
    for (int i = 0; i < n; ++i) squares += double(pcm[i * step]) * pcm[i * step];
    return std::sqrt(squares / n);
}
// How much of `pcm` is the note `hz` (Goertzel), 0 to about 1.
static double note(const std::vector<float> &pcm, double hz) {
    const double w = 2 * M_PI * hz / voice::kRate, k = 2 * std::cos(w);
    double a = 0, b = 0, squares = 1e-12;
    for (float v : pcm) {
        const double c = v + k * a - b;
        b = a, a = c, squares += double(v) * v;
    }
    return (a * a + b * b - k * a * b) * 2 / (double(pcm.size()) * squares);
}

static void codec() {
    voice::Encoder enc;
    voice::Decoder dec;
    uint8_t opus[proto::kMostVoice];
    float out[kFrame];
    check("the encoder and the decoder open", enc.Open() && dec.Open() && dec.open());
    std::vector<std::vector<uint8_t>> packets;
    std::vector<float> heard;
    size_t longest = 0;
    for (int i = 0; i < 50; ++i) {
        const size_t n = enc.Encode(hum(i).data(), opus, sizeof opus);
        packets.emplace_back(opus, opus + n);
        longest = n > longest ? n : longest;
        if (n && dec.Decode(opus, n, out) && i >= 25) heard.insert(heard.end(), out, out + kFrame);
    }
    size_t sent = 0;
    for (const auto &p : packets) sent += !p.empty();
    check("a second of a voice is fifty packets, each well under what a datagram's voice may be", sent == 50 && longest < 200);
    check("...and comes out as the note it was, about as loud", heard.size() == 25 * size_t(kFrame) && note(heard, 220) > 0.4 && note(heard, 330) < 0.05 &&
                                                               rms(heard.data(), int(heard.size())) > 0.08 && rms(heard.data(), int(heard.size())) < 0.3);
    const std::vector<float> quiet(kFrame, 0.0f);
    sent = 0;
    for (int i = 0; i < 100; ++i) sent += enc.Encode(quiet.data(), opus, sizeof opus) > 0;
    check("silence stops being sent after a moment", sent < 40);
    sent = 0;
    for (int i = 0; i < 20; ++i) sent += enc.Encode(hum(100 + i).data(), opus, sizeof opus) > 0;
    check("...and a voice that comes back is sent again", sent >= 18);

    // a lost packet
    voice::Decoder lossy;
    lossy.Open();
    for (int i = 0; i < 30; ++i) lossy.Decode(packets[size_t(i)].data(), packets[size_t(i)].size(), out);
    check("a lost packet made up from the one after it sounds like the voice", lossy.FromNext(packets[31].data(), packets[31].size(), out) && rms(out, kFrame) > 0.03);
    lossy.Decode(packets[31].data(), packets[31].size(), out);
    check("one filled in from what came before does too", lossy.Fill(out) && rms(out, kFrame) > 0.01);

    // what a peer might send instead of a voice
    voice::Decoder hostile;
    hostile.Open();
    bool quietOnBad = true;
    unsigned seed = 7;
    for (int i = 0; i < 4000; ++i) {
        uint8_t bytes[proto::kMostVoice];
        const size_t n = 1 + (seed = seed * 1103515245u + 12345u) % proto::kMostVoice;
        for (size_t k = 0; k < n; ++k) bytes[k] = uint8_t((seed = seed * 1103515245u + 12345u) >> 16);
        float room[kFrame + 8];
        for (float &v : room) v = 77;
        const bool ok = i % 2 ? hostile.Decode(bytes, n, room) : hostile.FromNext(bytes, n, room);
        for (int k = 0; k < kFrame; ++k) quietOnBad = quietOnBad && std::isfinite(room[k]) && (ok || room[k] == 0);
        for (int k = kFrame; k < kFrame + 8; ++k) quietOnBad = quietOnBad && room[k] == 77;   // nothing past the 20 ms
    }
    check("noise for a packet gives 20 ms and no more, silence when it doesn't decode", quietOnBad);
    check("a packet that isn't 20 ms isn't decoded", hostile.Decode(packets[40].data(), packets[40].size(), out) && [&] {
        const uint8_t sixty[3] = {uint8_t((packets[40][0] & 0xfc) | 3), 3, 0};   // code 3: three frames, the rest missing
        return !hostile.Decode(sixty, sizeof sixty, out) && rms(out, kFrame) == 0;
    }());
    check("no packet at all is silence", !hostile.Decode(nullptr, 0, out) && !hostile.Decode(opus, 0, out) && rms(out, kFrame) == 0);
    voice::Decoder closed;
    check("a decoder that isn't open gives silence", !closed.Decode(packets[3].data(), packets[3].size(), out) && !closed.Fill(out) && rms(out, kFrame) == 0);
}

// A second of `who` humming, into the mix as it would come from a link; what the ears get back.
struct Ear {
    double left = 0, right = 0;
};
static Ear say(voice::Voices &voices, const std::vector<int> &who, uint16_t from, int frames = 50) {
    static voice::Encoder enc[voice::kVoices];
    uint8_t opus[proto::kMostVoice];
    float stereo[2 * kFrame];
    Ear ear;
    for (int i = 0; i < frames; ++i) {
        for (int p : who) {
            if (i == 0) enc[p].Open();
            const size_t n = enc[p].Encode(hum(i, 200 + 60 * p).data(), opus, sizeof opus);
            const std::vector<uint8_t> d = proto::PackVoice(uint16_t(from + i), opus, n);
            if (!d.empty()) voices.Heard(p, d.data(), d.size());
        }
        voices.Mix(stereo);
        if (i >= frames / 2) ear.left += rms(stereo, kFrame, 2) / (frames - frames / 2), ear.right += rms(stereo + 1, kFrame, 2) / (frames - frames / 2);
    }
    return ear;
}

static void voices() {
    voice::Voices v;
    float stereo[2 * kFrame];
    v.Mix(stereo);
    check("with nobody speaking the mix is silence", rms(stereo, 2 * kFrame) == 0 && !v.told(1).speaking);
    Ear e = say(v, {1}, 100);
    check("someone whose place isn't known is heard in the middle", e.left > 0.02 && std::fabs(e.left - e.right) < 1e-3 * e.left);
    voice::Voices::Told t = v.told(1);
    check("...and is told of: speaking, how loud, how many packets", t.speaking && !t.placed && t.level > 0.1f && t.played >= 45 && t.late == 0);

    const double me[3] = {0, 1.6, 0};
    const float right[3] = {1.5f, 1.6f, 0}, left[3] = {-1.5f, 1.6f, 0}, far[3] = {0, 1.6f, -6};
    v.Listener(true, me, 0);
    v.Place(1, true, right);
    e = say(v, {1}, 200);
    check("placed to the right, they are louder in the right ear", e.right > 2 * e.left && e.left > 0.002 && v.told(1).placed);
    v.Listener(true, me, M_PI);
    e = say(v, {1}, 300);
    check("the listener turns round, and they are on the left", e.left > 2 * e.right);
    v.Listener(false, me, 0);
    e = say(v, {1}, 400);
    check("while the listener's own place isn't known, everyone is in the middle", std::fabs(e.left - e.right) < 0.05 * e.left);

    v.Listener(true, me, 0);
    v.Place(2, true, left);
    const Ear both = say(v, {1, 2}, 500);
    check("two people, one each side, are both heard", both.left > 0.02 && both.right > 0.02 && std::fabs(both.left - both.right) < 0.5 * both.left);
    v.Place(2, true, far);
    v.Place(1, false, right);
    const Ear near = say(v, {1}, 600), away = say(v, {2}, 700);
    check("someone far away is quieter than someone near", away.left < 0.5 * near.left && away.left > 0.001);

    v.Forget(2);
    v.Mix(stereo);
    check("someone who left is silent at once, and isn't told of", !v.told(2).speaking && v.told(2).played == 0 && !v.told(2).placed);
    const std::vector<uint8_t> d = proto::PackVoice(1, reinterpret_cast<const uint8_t *>("abc"), 3);
    check("what isn't a voice datagram, or is of a place there isn't, isn't taken", !v.Heard(1, d.data(), 2) && !v.Heard(4, d.data(), d.size()) && !v.Heard(-1, d.data(), d.size()) &&
                                                                                       v.Heard(3, d.data(), d.size()));
    bool finite = true;
    for (int i = 0; i < 10; ++i) {
        v.Mix(stereo);
        for (float s : stereo) finite = finite && std::isfinite(s) && std::fabs(s) <= 1;
    }
    check("...and rubbish for a voice mixes to something finite", finite);
    check("places there aren't are told of as nobody", !v.told(9).speaking && (v.Place(9, true, right), v.Forget(-3), true));
}

// The wearer's mouth over `frames` 20 ms turns of a hum this loud (0: silence), counted on from
// `from`: the datagrams there were to hand ft-together. `twin` hears the same, to tell what
// the mouth's weights should be.
struct Said {
    int told = 0, open = 0;   // datagrams; those with the mouth open
    int widest = 0;           // the mouth's shapes' sum at its most
    bool right = true;        // each was a mouth datagram with the weights the voice makes, 255 whole
    int last = -1;            // the last one's sum
};
static Said speak(voice::MouthSay &mouth, face::Mouth &twin, int from, int frames, double loud) {
    Said said;
    for (int i = 0; i < frames; ++i) {
        const std::vector<float> pcm = loud > 0 ? hum(from + i, 220, loud) : std::vector<float>(kFrame, 0.0f);
        float want[face::kWeights] = {};
        twin.Hear(pcm.data(), pcm.size(), voice::kRate);
        twin.After(0.02, want);
        uint8_t out[local::kMouthBytes];
        if (!mouth.Hear(pcm.data(), pcm.size(), voice::kRate, out)) continue;
        int sum = 0;
        said.right = said.right && out[0] == local::kMouthToTogether;
        for (int k = 0; k < proto::kMouthShapes; ++k) {
            sum += out[1 + k];
            said.right = said.right && out[1 + k] == std::lround(want[bodyfile::Shape::kAa + k] * 255);
        }
        ++said.told, said.open += sum > 0, said.widest = std::max(said.widest, sum), said.last = sum;
    }
    return said;
}

static void mouths() {
    voice::MouthSay mouth;
    face::Mouth twin;
    uint8_t out[local::kMouthBytes];
    check("a wearer who says nothing has nothing told of their mouth", speak(mouth, twin, 0, 100, 0).told == 0 && !mouth.Shut(out));
    twin = face::Mouth();
    const Said spoke = speak(mouth, twin, 100, 100, 0.3);
    check("one who speaks has it told every 20 ms, open", spoke.told >= 98 && spoke.open == spoke.told && spoke.widest > 100 && spoke.widest <= 255 + proto::kMouthShapes);
    check("...as the weights their voice makes of the five shapes, 255 whole", spoke.right);
    const Said after = speak(mouth, twin, 200, 100, 0);
    check("when they stop it shuts, is told shut once, and then nothing more is said",
          after.told >= 2 && after.told < 60 && after.open == after.told - 1 && after.last == 0 && after.right);
    check("...until they speak again", speak(mouth, twin, 300, 50, 0).told == 0 && speak(mouth, twin, 350, 50, 0.3).open > 30);
    const bool told = mouth.Shut(out);
    int sum = 0;
    for (size_t k = 1; k < sizeof out; ++k) sum += out[k];
    check("the microphone closes on an open mouth: it is told shut, once", told && out[0] == local::kMouthToTogether && sum == 0 && !mouth.Shut(out));
    twin = face::Mouth();
    const Said again = speak(mouth, twin, 400, 50, 0.3);
    check("...and when it opens again the mouth starts afresh", again.right && again.open > 30);
    std::vector<float> bad(kFrame, std::nanf(""));
    bad[3] = INFINITY;
    mouth.Shut(out);
    check("what isn't sound moves nothing", !mouth.Hear(bad.data(), bad.size(), voice::kRate, out) && !mouth.Hear(bad.data(), 0, voice::kRate, out) &&
                                                !mouth.Hear(bad.data(), bad.size(), 0, out));
}

int main() {
    jitter();
    spatial();
    codec();
    voices();
    mouths();
    std::printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
