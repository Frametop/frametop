// A voice to and from Opus packets of 20 ms (docs/together.md, "The protocol"): one channel,
// 48 kHz, 960 samples a packet.
//
// The decoder is handed what a peer sent. libopus takes any bytes; what it is asked to decode
// here is only a packet that says it holds exactly 20 ms, so a peer can't make it write more
// than a frame.
#pragma once

#include <cstddef>
#include <cstdint>

struct OpusEncoder;
struct OpusDecoder;

namespace voice {

constexpr int kRate = 48000;
constexpr int kFrame = kRate / 50;   // samples in 20 ms

class Encoder {
public:
    static constexpr int kBitrate = 24000;
    bool Open();
    void Close();
    // 20 ms of voice as a packet in `out` (room for proto::kMostVoice). Its bytes; 0 when this
    // one needn't be sent (silence) or didn't encode.
    size_t Encode(const float *pcm, uint8_t *out, size_t most);
    Encoder() = default;
    Encoder(const Encoder &) = delete;
    Encoder &operator=(const Encoder &) = delete;
    ~Encoder() { Close(); }

private:
    OpusEncoder *enc_ = nullptr;
};

class Decoder {
public:
    bool Open();
    void Close();
    bool open() const { return dec_ != nullptr; }
    // Each fills `out` with kFrame samples. False (and silence) when it couldn't.
    bool Decode(const uint8_t *opus, size_t size, float *out);     // this packet
    bool FromNext(const uint8_t *opus, size_t size, float *out);   // the lost one before this packet
    bool Fill(float *out);                                         // a lost one, from what came before
    Decoder() = default;
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    ~Decoder() { Close(); }

private:
    bool Run(const uint8_t *opus, size_t size, float *out, int fec);
    OpusDecoder *dec_ = nullptr;
};

}  // namespace voice
