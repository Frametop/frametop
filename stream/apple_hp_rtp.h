// Apple High Performance Screen Sharing client (MIT), part of apple_hp.h.
// UDP 5900/5901 media transport: RFC 3711 SRTP / SRTCP, RTCP feedback, and RFC 3640 AAC-ELD
// access units.
#pragma once

#include "apple_hp_base.h"

namespace ft_apple_hp {

// ── RFC 3711 SRTP / SRTCP (AES_CM_256 + HMAC-SHA1-80) & RTCP Feedback ────────

inline bool SrtpKdf(const uint8_t master_key[32], const uint8_t master_salt[14],
                    uint8_t label, uint8_t *out, size_t out_len) {
    uint8_t iv[16]{};
    std::memcpy(iv, master_salt, 14);
    iv[7] ^= label;
    std::vector<uint8_t> zeros(out_len, 0);
    return Aes256CtrCrypt(master_key, iv, zeros.data(), out_len, out);
}

class SrtpContext {
public:
    bool InitFromBlob(const uint8_t blob46[46]) {
        const uint8_t *mk = blob46;
        const uint8_t *ms = blob46 + 32;
        if (!SrtpKdf(mk, ms, 0, rtp_key_.data(), 32) ||
            !SrtpKdf(mk, ms, 1, rtp_auth_.data(), 20) ||
            !SrtpKdf(mk, ms, 2, rtp_salt_.data(), 14) ||
            !SrtpKdf(mk, ms, 3, rtcp_key_.data(), 32) ||
            !SrtpKdf(mk, ms, 4, rtcp_auth_.data(), 20) ||
            !SrtpKdf(mk, ms, 5, rtcp_salt_.data(), 14)) {
            return false;
        }
        ssrc_states_.clear();
        srtcp_tx_index_ = 0;
        init_ = true;
        return true;
    }

    struct RtpHeader {
        uint8_t pt = 0;
        bool marker = false;
        uint16_t seq = 0;
        uint32_t timestamp = 0;
        uint32_t ssrc = 0;
        uint64_t pkt_index = 0;
        size_t header_len = 0;
    };

    bool DecryptRtp(const uint8_t *pkt, size_t len, RtpHeader *hdr_out, std::vector<uint8_t> *payload_out) {
        if (!init_ || len < 12 + kSrtpTagLen) return false;
        const uint8_t b0 = pkt[0];
        if ((b0 >> 6) != 2) return false;
        const size_t csrc_cnt = b0 & 0x0f;
        const bool has_pad = (b0 & 0x20) != 0;
        const bool has_ext = (b0 & 0x10) != 0;
        size_t hlen = 12 + 4 * csrc_cnt;
        const size_t auth_portion_len = len - kSrtpTagLen;
        if (hlen > auth_portion_len) return false;
        if (has_ext) {
            if (hlen + 4 > auth_portion_len) return false;
            const uint16_t ext_words = ReadBe16(pkt + hlen + 2);
            hlen += 4 + 4 * size_t(ext_words);
            if (hlen > auth_portion_len) return false;
        }

        const uint16_t seq = ReadBe16(pkt + 2);
        const uint32_t ts = ReadBe32(pkt + 4);
        const uint32_t ssrc = ReadBe32(pkt + 8);

        // Look up without inserting: spoofed packets mustn't grow the map.
        SsrcState fresh{};
        const auto found = ssrc_states_.find(ssrc);
        SsrcState *stp = found != ssrc_states_.end() ? &found->second : &fresh;
        SsrcState &probe = *stp;
        uint32_t est_roc = probe.roc;
        int32_t delta_roc = 0;
        if (!probe.initialized) {
            est_roc = 0;
        } else if (probe.max_seq < 32768) {
            if (int32_t(seq) - int32_t(probe.max_seq) > 32768 && probe.roc > 0) {
                est_roc = probe.roc - 1;
                delta_roc = -1;
            }
        } else {
            if (int32_t(probe.max_seq) - 32768 > int32_t(seq)) {
                est_roc = probe.roc + 1;
                delta_roc = 1;
            }
        }

        uint8_t roc_be[4], mac20[20];
        WriteBe32(roc_be, est_roc);
        if (!HmacSha1(rtp_auth_.data(), rtp_auth_.size(), pkt, auth_portion_len, roc_be, 4, mac20)) return false;
        if (CRYPTO_memcmp(mac20, pkt + auth_portion_len, kSrtpTagLen) != 0) return false;

        SsrcState &st = stp == &fresh ? ssrc_states_[ssrc] : *stp;
        if (!st.initialized) {
            st.initialized = true;
            st.max_seq = seq;
            st.roc = 0;
        } else if (delta_roc == 1) {
            st.roc = est_roc;
            st.max_seq = seq;
        } else if (delta_roc == 0 && seq > st.max_seq) {
            st.max_seq = seq;
        }

        const uint64_t idx = (uint64_t(est_roc) << 16) | uint64_t(seq);
        uint8_t iv[16]{};
        std::memcpy(iv, rtp_salt_.data(), 14);
        iv[4] ^= pkt[8];
        iv[5] ^= pkt[9];
        iv[6] ^= pkt[10];
        iv[7] ^= pkt[11];
        for (int i = 0; i < 6; ++i) iv[8 + i] ^= uint8_t((idx >> (40 - 8 * i)) & 0xff);

        const size_t pay_len = auth_portion_len - hlen;
        payload_out->resize(pay_len);
        if (pay_len > 0 && !Aes256CtrCrypt(rtp_key_.data(), iv, pkt + hlen, pay_len, payload_out->data())) return false;
        if (has_pad) {
            if (payload_out->empty()) return false;
            const uint8_t pad_bytes = payload_out->back();
            if (pad_bytes == 0 || pad_bytes > payload_out->size()) return false;
            payload_out->resize(payload_out->size() - pad_bytes);
        }

        hdr_out->pt = pkt[1] & 0x7f;
        hdr_out->marker = (pkt[1] & 0x80) != 0;
        hdr_out->seq = seq;
        hdr_out->timestamp = ts;
        hdr_out->ssrc = ssrc;
        hdr_out->pkt_index = idx;
        hdr_out->header_len = hlen;
        return true;
    }

    bool EncryptRtp(const uint8_t *rtp_plain, size_t len, uint32_t roc, std::vector<uint8_t> *srtp_out) const {
        if (!init_ || len < 12) return false;
        const size_t csrc_cnt = rtp_plain[0] & 0x0f;
        const bool has_ext = (rtp_plain[0] & 0x10) != 0;
        size_t hlen = 12 + 4 * csrc_cnt;
        if (hlen > len) return false;
        if (has_ext) {
            if (hlen + 4 > len) return false;
            hlen += 4 + 4 * size_t(ReadBe16(rtp_plain + hlen + 2));
            if (hlen > len) return false;
        }
        const uint16_t seq = ReadBe16(rtp_plain + 2);
        const uint64_t idx = (uint64_t(roc) << 16) | uint64_t(seq);
        uint8_t iv[16]{};
        std::memcpy(iv, rtp_salt_.data(), 14);
        for (int i = 0; i < 4; ++i) iv[4 + i] ^= rtp_plain[8 + i];
        for (int i = 0; i < 6; ++i) iv[8 + i] ^= uint8_t((idx >> (40 - 8 * i)) & 0xff);

        srtp_out->resize(len + kSrtpTagLen);
        std::memcpy(srtp_out->data(), rtp_plain, hlen);
        if (len > hlen &&
            !Aes256CtrCrypt(rtp_key_.data(), iv, rtp_plain + hlen, len - hlen, srtp_out->data() + hlen)) {
            return false;
        }
        uint8_t roc_be[4], mac20[20];
        WriteBe32(roc_be, roc);
        if (!HmacSha1(rtp_auth_.data(), rtp_auth_.size(), srtp_out->data(), len, roc_be, 4, mac20)) return false;
        std::memcpy(srtp_out->data() + len, mac20, kSrtpTagLen);
        return true;
    }

    bool EncryptRtcp(const uint8_t *rtcp_plain, size_t len, std::vector<uint8_t> *srtcp_out) {
        if (!init_ || len < 8) return false;
        const uint32_t idx = srtcp_tx_index_++ & 0x7fffffffu;
        uint8_t iv[16]{};
        std::memcpy(iv, rtcp_salt_.data(), 14);
        for (int i = 0; i < 4; ++i) iv[4 + i] ^= rtcp_plain[4 + i];
        iv[10] ^= uint8_t((idx >> 24) & 0x7f);
        iv[11] ^= uint8_t((idx >> 16) & 0xff);
        iv[12] ^= uint8_t((idx >> 8) & 0xff);
        iv[13] ^= uint8_t(idx & 0xff);

        srtcp_out->resize(len + 4 + kSrtpTagLen);
        std::memcpy(srtcp_out->data(), rtcp_plain, 8);
        if (len > 8 &&
            !Aes256CtrCrypt(rtcp_key_.data(), iv, rtcp_plain + 8, len - 8, srtcp_out->data() + 8)) {
            return false;
        }
        WriteBe32(srtcp_out->data() + len, 0x80000000u | idx);
        uint8_t mac20[20];
        if (!HmacSha1(rtcp_auth_.data(), rtcp_auth_.size(), srtcp_out->data(), len + 4, nullptr, 0, mac20))
            return false;
        std::memcpy(srtcp_out->data() + len + 4, mac20, kSrtpTagLen);
        return true;
    }

    bool DecryptRtcp(const uint8_t *srtcp, size_t len, std::vector<uint8_t> *rtcp_out) const {
        if (!init_ || len < 8 + 4 + kSrtpTagLen) return false;
        const size_t body_and_idx = len - kSrtpTagLen;
        uint8_t mac20[20];
        if (!HmacSha1(rtcp_auth_.data(), rtcp_auth_.size(), srtcp, body_and_idx, nullptr, 0, mac20)) return false;
        if (CRYPTO_memcmp(mac20, srtcp + body_and_idx, kSrtpTagLen) != 0) return false;

        const size_t rtcp_len = body_and_idx - 4;
        const uint32_t e_and_idx = ReadBe32(srtcp + rtcp_len);
        const bool encrypted = (e_and_idx & 0x80000000u) != 0;
        const uint32_t idx = e_and_idx & 0x7fffffffu;

        rtcp_out->resize(rtcp_len);
        std::memcpy(rtcp_out->data(), srtcp, 8);
        if (rtcp_len > 8) {
            if (encrypted) {
                uint8_t iv[16]{};
                std::memcpy(iv, rtcp_salt_.data(), 14);
                for (int i = 0; i < 4; ++i) iv[4 + i] ^= srtcp[4 + i];
                iv[10] ^= uint8_t((idx >> 24) & 0x7f);
                iv[11] ^= uint8_t((idx >> 16) & 0xff);
                iv[12] ^= uint8_t((idx >> 8) & 0xff);
                iv[13] ^= uint8_t(idx & 0xff);
                if (!Aes256CtrCrypt(rtcp_key_.data(), iv, srtcp + 8, rtcp_len - 8, rtcp_out->data() + 8)) return false;
            } else {
                std::memcpy(rtcp_out->data() + 8, srtcp + 8, rtcp_len - 8);
            }
        }
        return true;
    }

private:
    struct SsrcState {
        uint32_t roc = 0;
        uint16_t max_seq = 0;
        bool initialized = false;
    };
    bool init_ = false;
    std::array<uint8_t, 32> rtp_key_{};
    std::array<uint8_t, 20> rtp_auth_{};
    std::array<uint8_t, 14> rtp_salt_{};
    std::array<uint8_t, 32> rtcp_key_{};
    std::array<uint8_t, 20> rtcp_auth_{};
    std::array<uint8_t, 14> rtcp_salt_{};
    std::map<uint32_t, SsrcState> ssrc_states_;
    uint32_t srtcp_tx_index_ = 0;
};

inline std::vector<uint8_t> BuildEmptySr(uint32_t sender_ssrc) {
    constexpr uint64_t kNtpDelta = 2208988800ULL;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    const uint32_t ntp_sec = uint32_t((us / 1'000'000ULL) + kNtpDelta);
    const uint32_t ntp_frac = uint32_t(((us % 1'000'000ULL) << 32) / 1'000'000ULL);
    const uint32_t rtp_ts = uint32_t((us * 90ULL) / 1000ULL);

    std::vector<uint8_t> p = {0x80, 200, 0x00, 0x06};
    AppendBe32(p, sender_ssrc);
    AppendBe32(p, ntp_sec);
    AppendBe32(p, ntp_frac);
    AppendBe32(p, rtp_ts);
    AppendBe32(p, 0);
    AppendBe32(p, 0);
    return p;
}

inline std::vector<uint8_t> BuildEmptyRr(uint32_t sender_ssrc) {
    std::vector<uint8_t> p = {0x80, 201, 0x00, 0x01};
    AppendBe32(p, sender_ssrc);
    return p;
}

inline std::vector<uint8_t> BuildFirLegacy(uint32_t target_ssrc) {
    std::vector<uint8_t> p = {0x80, 192, 0x00, 0x01};
    AppendBe32(p, target_ssrc);
    return p;
}

inline std::vector<uint8_t> BuildPli(uint32_t sender_ssrc, uint32_t media_ssrc) {
    std::vector<uint8_t> p = {0x81, 206, 0x00, 0x02};
    AppendBe32(p, sender_ssrc);
    AppendBe32(p, media_ssrc);
    return p;
}

inline std::vector<uint8_t> BuildNack(uint32_t sender_ssrc, uint32_t media_ssrc,
                                      const std::set<uint16_t> &lost_seqs) {
    if (lost_seqs.empty()) return {};
    std::vector<uint16_t> seqs(lost_seqs.begin(), lost_seqs.end());
    std::vector<uint8_t> fcis;
    for (size_t i = 0; i < seqs.size();) {
        const uint16_t pid = seqs[i];
        uint16_t blp = 0;
        size_t j = i + 1;
        while (j < seqs.size()) {
            const uint16_t diff = uint16_t(seqs[j] - pid);
            if (diff >= 1 && diff <= 16) {
                blp |= uint16_t(1u << (diff - 1));
                ++j;
            } else {
                break;
            }
        }
        AppendBe16(fcis, pid);
        AppendBe16(fcis, blp);
        i = j;
    }
    const uint16_t len_words = uint16_t(2 + fcis.size() / 4);
    std::vector<uint8_t> p = {0x81, 205};
    AppendBe16(p, len_words);
    AppendBe32(p, sender_ssrc);
    AppendBe32(p, media_ssrc);
    p.insert(p.end(), fcis.begin(), fcis.end());
    return p;
}

// ── RFC 3640 AAC-ELD RTP Depacketizer ─────────────────────────────────────────

// AVCAudioStream payload 101 (AAC-ELD / AAC-ELD-SBR) frames RTP payloads using RFC 3640
// AU-headers ([au_headers_length_bits:16be][au_size:13 | au_index:3]...[au_data...]).
// Falls back cleanly to a single raw AU if the header does not match payload bounds.
inline void ParseRfc3640AuPayloads(const uint8_t *pay, size_t len, std::vector<std::vector<uint8_t>> *out_aus) {
    if (!out_aus || len == 0) return;
    if (len >= 4) {
        const uint16_t hdr_bits = ReadBe16(pay);
        if (hdr_bits >= 16 && (hdr_bits % 16) == 0) {
            const size_t n_aus = hdr_bits / 16;
            const size_t hdr_bytes = 2 + 2 * n_aus;
            if (hdr_bytes < len) {
                size_t total = 0;
                std::vector<size_t> sizes(n_aus);
                for (size_t i = 0; i < n_aus; ++i) {
                    sizes[i] = size_t(ReadBe16(pay + 2 + 2 * i) >> 3);
                    total += sizes[i];
                }
                if (hdr_bytes + total == len && total > 0) {
                    size_t off = hdr_bytes;
                    for (size_t sz : sizes) {
                        if (sz > 0) out_aus->emplace_back(pay + off, pay + off + sz);
                        off += sz;
                    }
                    return;
                }
            }
        }
    }
    out_aus->emplace_back(pay, pay + len);
}

}  // namespace ft_apple_hp
