// Opus for a voice (codec.h).
#include "codec.h"

#include <opus/opus.h>

#include <algorithm>

namespace voice {

namespace {
const uint8_t kNone[1] = {};   // stands for a packet that isn't there: nothing is decoded from it
}

bool Encoder::Open() {
    Close();
    int err = 0;
    enc_ = opus_encoder_create(kRate, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc_) return false;
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));       // a Frame has little to spare
    opus_encoder_ctl(enc_, OPUS_SET_DTX(1));              // silence isn't sent
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));       // each packet carries a thin copy of the one before
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(10));
    return true;
}

void Encoder::Close() {
    if (enc_) opus_encoder_destroy(enc_);
    enc_ = nullptr;
}

size_t Encoder::Encode(const float *pcm, uint8_t *out, size_t most) {
    if (!enc_) return 0;
    const int n = opus_encode_float(enc_, pcm, kFrame, out, opus_int32(most));
    return n > 2 ? size_t(n) : 0;   // two bytes or fewer: nothing to send (opus_encode)
}

bool Decoder::Open() {
    Close();
    int err = 0;
    dec_ = opus_decoder_create(kRate, 1, &err);
    return dec_ != nullptr;
}

void Decoder::Close() {
    if (dec_) opus_decoder_destroy(dec_);
    dec_ = nullptr;
}

// `opus`: a packet, or nullptr for the decoder to fill in. A packet that isn't 20 ms isn't
// decoded.
bool Decoder::Run(const uint8_t *opus, size_t size, float *out, int fec) {
    const bool whole = dec_ && (!opus || (size && opus_packet_get_nb_samples(opus, opus_int32(size), kRate) == kFrame));
    if (whole && opus_decode_float(dec_, opus, opus ? opus_int32(size) : 0, out, kFrame, fec) == kFrame) return true;
    std::fill(out, out + kFrame, 0.0f);
    return false;
}

bool Decoder::Decode(const uint8_t *opus, size_t size, float *out) { return Run(opus ? opus : kNone, opus ? size : 0, out, 0); }
bool Decoder::FromNext(const uint8_t *opus, size_t size, float *out) { return Run(opus ? opus : kNone, opus ? size : 0, out, 1); }
bool Decoder::Fill(float *out) { return Run(nullptr, 0, out, 0); }

}  // namespace voice
