// Apple High Performance Screen Sharing client (MIT), part of apple_hp.h.
// The TCP 5900 control channel's messages: bplist00 and protobuf offers for
// AVCMediaStreamNegotiator (0x1c), the enc1103 record cipher, RFB control messages, and 0x451's
// display layout.
#pragma once

#include "apple_hp_base.h"

namespace ft_apple_hp {

// ── Minimal Binary Plist (bplist00) Builder & Reader ─────────────────────────

inline void AppendBplistLength(std::vector<uint8_t> &b, uint8_t marker_hi, size_t len) {
    if (len < 15) {
        b.push_back(uint8_t((marker_hi << 4) | len));
    } else {
        b.push_back(uint8_t((marker_hi << 4) | 0x0f));
        if (len <= 0xff) {
            b.push_back(0x10);
            b.push_back(uint8_t(len));
        } else if (len <= 0xffff) {
            b.push_back(0x11);
            AppendBe16(b, uint16_t(len));
        } else {
            b.push_back(0x12);
            AppendBe32(b, uint32_t(len));
        }
    }
}

inline void AppendBplistAscii(std::vector<uint8_t> &b, const std::string &s) {
    AppendBplistLength(b, 0x5, s.size());
    b.insert(b.end(), s.begin(), s.end());
}

inline void AppendBplistData(std::vector<uint8_t> &b, const std::vector<uint8_t> &d) {
    AppendBplistLength(b, 0x4, d.size());
    b.insert(b.end(), d.begin(), d.end());
}

inline std::vector<uint8_t> BuildOfferBplist(int mode, const std::vector<uint8_t> &zlib_blob,
                                             const std::string &call_id) {
    std::vector<uint8_t> out = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    std::array<uint32_t, 7> offs{};

    // Object 0: dict with 3 key/value pairs (refs 1,2,3 -> 4,5,6).
    offs[0] = uint32_t(out.size());
    out.insert(out.end(), {0xd3, 1, 2, 3, 4, 5, 6});

    offs[1] = uint32_t(out.size());
    AppendBplistAscii(out, "avcMediaStreamNegotiatorMediaBlob");
    offs[2] = uint32_t(out.size());
    AppendBplistAscii(out, "avcMediaStreamNegotiatorMode");
    offs[3] = uint32_t(out.size());
    AppendBplistAscii(out, "avcMediaStreamOptionCallID");

    offs[4] = uint32_t(out.size());
    AppendBplistData(out, zlib_blob);
    offs[5] = uint32_t(out.size());
    out.push_back(0x10);
    out.push_back(uint8_t(mode & 0xff));
    offs[6] = uint32_t(out.size());
    AppendBplistAscii(out, call_id);

    const uint64_t off_table_pos = out.size();
    for (uint32_t o : offs) AppendBe16(out, uint16_t(o));

    // 32-byte trailer: 6 pad, offsetIntSize=2, objectRefSize=1, numObjects=7, topObject=0, offsetTableOffset.
    for (int i = 0; i < 6; ++i) out.push_back(0);
    out.push_back(2);
    out.push_back(1);
    AppendBe64(out, 7);
    AppendBe64(out, 0);
    AppendBe64(out, off_table_pos);
    return out;
}

inline bool ReadBplistUintN(const uint8_t *p, size_t n, uint64_t *out) {
    if (n == 0 || n > 8) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    *out = v;
    return true;
}

inline bool ReadBplistSizedHeader(const uint8_t *buf, size_t len, size_t pos,
                                  uint8_t expected_hi, size_t *payload_pos, size_t *payload_len) {
    if (pos >= len) return false;
    const uint8_t b = buf[pos++];
    if ((b >> 4) != expected_hi) return false;
    const uint8_t lo = b & 0x0f;
    if (lo != 0x0f) {
        *payload_len = lo;
        *payload_pos = pos;
        return pos + lo <= len;
    }
    if (pos >= len || (buf[pos] >> 4) != 0x1) return false;
    const size_t int_bytes = size_t(1u) << (buf[pos] & 0x0f);
    ++pos;
    if (int_bytes > 8 || pos + int_bytes > len) return false;
    uint64_t sz = 0;
    if (!ReadBplistUintN(buf + pos, int_bytes, &sz)) return false;
    pos += int_bytes;
    if (sz > len - pos) return false;
    *payload_pos = pos;
    *payload_len = size_t(sz);
    return true;
}

inline bool ParseBplistMediaBlob(const uint8_t *buf, size_t len, std::vector<uint8_t> *out_zlib) {
    if (len < 40 || std::memcmp(buf, "bplist00", 8) != 0) return false;
    const uint8_t *tr = buf + len - 32;
    const uint8_t off_sz = tr[6];
    const uint8_t ref_sz = tr[7];
    const uint64_t num_objs = ReadBe64(tr + 8);
    const uint64_t top_obj = ReadBe64(tr + 16);
    const uint64_t off_tbl = ReadBe64(tr + 24);
    if (off_sz < 1 || off_sz > 8 || ref_sz < 1 || ref_sz > 8) return false;
    if (num_objs == 0 || num_objs > 4096 || top_obj >= num_objs) return false;
    // Compare without multiplying first: off_tbl and num_objs come from the peer.
    if (off_tbl < 8 || off_tbl > len - 32 || num_objs > (len - 32 - off_tbl) / off_sz) return false;

    auto obj_offset = [&](uint64_t ref, size_t *out_off) -> bool {
        if (ref >= num_objs) return false;
        uint64_t v = 0;
        if (!ReadBplistUintN(buf + off_tbl + ref * off_sz, off_sz, &v) || v >= off_tbl) return false;
        *out_off = size_t(v);
        return true;
    };

    size_t top_pos = 0;
    if (!obj_offset(top_obj, &top_pos) || top_pos >= off_tbl) return false;
    size_t refs_pos = 0, dict_cnt = 0;
    if ((buf[top_pos] >> 4) != 0xd) return false;
    if ((buf[top_pos] & 0x0f) != 0x0f) {
        dict_cnt = buf[top_pos] & 0x0f;
        refs_pos = top_pos + 1;
    } else {
        // Count encoded as integer object immediately after 0xdf.
        size_t pos = top_pos + 1;
        if (pos >= off_tbl || (buf[pos] >> 4) != 0x1) return false;
        const size_t int_bytes = size_t(1u) << (buf[pos] & 0x0f);
        ++pos;
        if (int_bytes > 8 || pos + int_bytes > off_tbl) return false;
        uint64_t cnt64 = 0;
        if (!ReadBplistUintN(buf + pos, int_bytes, &cnt64) || cnt64 > 1024) return false;
        dict_cnt = size_t(cnt64);
        refs_pos = pos + int_bytes;
    }
    if (refs_pos + 2 * dict_cnt * ref_sz > off_tbl) return false;

    for (size_t i = 0; i < dict_cnt; ++i) {
        uint64_t kref = 0, vref = 0;
        if (!ReadBplistUintN(buf + refs_pos + i * ref_sz, ref_sz, &kref)) return false;
        if (!ReadBplistUintN(buf + refs_pos + (dict_cnt + i) * ref_sz, ref_sz, &vref)) return false;
        size_t kpos = 0, vpos = 0;
        if (!obj_offset(kref, &kpos) || !obj_offset(vref, &vpos)) return false;
        size_t str_pos = 0, str_len = 0;
        if (!ReadBplistSizedHeader(buf, off_tbl, kpos, 0x5, &str_pos, &str_len)) continue;
        const std::string key(reinterpret_cast<const char *>(buf + str_pos), str_len);
        if (key == "avcMediaStreamNegotiatorMediaBlob") {
            size_t data_pos = 0, data_len = 0;
            if (!ReadBplistSizedHeader(buf, off_tbl, vpos, 0x4, &data_pos, &data_len)) return false;
            out_zlib->assign(buf + data_pos, buf + data_pos + data_len);
            return true;
        }
    }
    return false;
}

// ── Protobuf Helpers & AVCMediaStreamNegotiator Offers (0x1c) ────────────────

inline void AppendVarint(std::vector<uint8_t> &b, uint64_t v) {
    while (v > 0x7f) {
        b.push_back(uint8_t((v & 0x7f) | 0x80));
        v >>= 7;
    }
    b.push_back(uint8_t(v));
}

inline void AppendPbVarint(std::vector<uint8_t> &b, uint32_t field_num, uint64_t val) {
    AppendVarint(b, (uint64_t(field_num) << 3) | 0u);
    AppendVarint(b, val);
}

inline void AppendPbBytes(std::vector<uint8_t> &b, uint32_t field_num, const uint8_t *data, size_t len) {
    AppendVarint(b, (uint64_t(field_num) << 3) | 2u);
    AppendVarint(b, len);
    b.insert(b.end(), data, data + len);
}

inline void AppendPbBytes(std::vector<uint8_t> &b, uint32_t field_num, const std::vector<uint8_t> &v) {
    AppendPbBytes(b, field_num, v.data(), v.size());
}

inline void AppendPbStr(std::vector<uint8_t> &b, uint32_t field_num, const std::string &s) {
    AppendPbBytes(b, field_num, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

inline bool ReadVarint(const uint8_t *data, size_t len, size_t *pos, uint64_t *val) {
    uint64_t v = 0;
    int shift = 0;
    while (*pos < len && shift < 64) {
        const uint8_t b = data[(*pos)++];
        v |= uint64_t(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            *val = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

enum class VideoCodec { kAvc, kHevc };

struct OfferConfig {
    VideoCodec codec = VideoCodec::kAvc;
    uint32_t tiles_per_frame = 1;
    bool ltrp = false;
    bool audio_enabled = false;
};

struct BuiltOffers {
    std::vector<uint8_t> video_bplist;
    std::vector<uint8_t> audio_bplist;
    uint32_t video_ssrc = 0;
    uint32_t audio_ssrc = 0;
};

inline std::vector<uint8_t> BuildAppleAudioF9Tiers() {
    struct Tier {
        uint32_t f1;
        uint32_t f2;
        int32_t f3;
    };
    static constexpr Tier kTiers[] = {
        {0, 40'000'000, 12288},   {0, 6'000'000, 131072}, {4074, 0, 16384},
        {16, 4100, -1},           {0, 75'000'000, 524288}, {0, 20'000'000, 98304},
        {4, 6500, -1},            {0, 60'000'000, 262144}, {1, 299, -1},
        {0, 100'000'000, 1048576},
    };
    std::vector<uint8_t> out;
    for (const auto &t : kTiers) {
        std::vector<uint8_t> body;
        AppendPbVarint(body, 1, t.f1);
        AppendPbVarint(body, 2, t.f2);
        if (t.f3 >= 0) AppendPbVarint(body, 3, uint64_t(t.f3));
        AppendPbBytes(out, 9, body);
    }
    return out;
}

inline std::vector<uint8_t> BuildMediaBlob(int mode, uint32_t session_id, uint32_t timestamp,
                                           const OfferConfig &cfg) {
    std::vector<uint8_t> out;
    AppendPbVarint(out, 1, 1);
    AppendPbVarint(out, 2, 1);

    if (mode == 7) {
        std::vector<uint8_t> res1, res2;
        AppendPbVarint(res1, 1, 1);
        AppendPbVarint(res1, 2, 1);
        AppendPbVarint(res1, 3, 50115);
        AppendPbVarint(res1, 4, 0);

        AppendPbVarint(res2, 1, 1);
        AppendPbVarint(res2, 2, 2);
        AppendPbVarint(res2, 3, 50115);
        AppendPbVarint(res2, 4, 0);

        const bool ltrp_on = (cfg.codec == VideoCodec::kHevc) && cfg.ltrp;
        const std::string h264_bank_params =
            ltrp_on ? "FLS;MS:-1;LF:-1;LTR;CABAC;POS:0;EOD:1;HTS:2;RR:3;AR:16/9,5/8;XR:16/9,5/8;"
                    : "FLS;MS:-1;LF:-1;CABAC;POS:0;EOD:1;HTS:2;RR:3;AR:16/9,5/8;XR:16/9,5/8;";
        const std::string hevc_bank_params =
            "FLS;LF:-1;POS:5;EOD:1;HTS:2;RR:3;POSE:4;AR:16/9,5/8;XR:16/9,5/8;";

        // Empirical AVConference response mapping:
        //   field1=123 yields H.264 4:2:0; field1=100 yields HEVC RExt 4:4:4.
        std::vector<uint8_t> bank_yields_h264;
        AppendPbVarint(bank_yields_h264, 1, 123);
        AppendPbBytes(bank_yields_h264, 2, res1);
        AppendPbBytes(bank_yields_h264, 2, res2);
        AppendPbBytes(bank_yields_h264, 2, res1);
        AppendPbBytes(bank_yields_h264, 2, res2);
        AppendPbStr(bank_yields_h264, 3, h264_bank_params);
        AppendPbVarint(bank_yields_h264, 4, 1);

        std::vector<uint8_t> bank_yields_hevc;
        AppendPbVarint(bank_yields_hevc, 1, 100);
        AppendPbBytes(bank_yields_hevc, 2, res1);
        AppendPbBytes(bank_yields_hevc, 2, res2);
        AppendPbStr(bank_yields_hevc, 3, hevc_bank_params);
        AppendPbVarint(bank_yields_hevc, 4, 14);

        std::vector<uint8_t> desc;
        AppendPbVarint(desc, 1, session_id);
        AppendPbVarint(desc, 2, ltrp_on ? 1 : 0);
        if (cfg.codec == VideoCodec::kAvc) {
            AppendPbBytes(desc, 3, bank_yields_h264);
        } else {
            AppendPbBytes(desc, 3, bank_yields_hevc);
        }
        AppendPbVarint(desc, 6, std::max<uint32_t>(1, cfg.tiles_per_frame));
        AppendPbVarint(desc, 7, ltrp_on ? 1 : 0);
        AppendPbVarint(desc, 8, 63);
        AppendPbVarint(desc, 9, 1);
        AppendPbVarint(desc, 12, 1);
        AppendPbBytes(out, 5, desc);
    } else {
        // Audio stream offer: < 5000 bps gates screensharingd's audio sender off cleanly.
        const uint32_t audio_f4 = cfg.audio_enabled ? 24191u : 1000u;
        std::vector<uint8_t> desc;
        AppendPbVarint(desc, 1, session_id);
        AppendPbVarint(desc, 2, 0);
        AppendPbVarint(desc, 3, 0);
        AppendPbVarint(desc, 4, audio_f4);
        AppendPbVarint(desc, 5, 0);
        AppendPbVarint(desc, 6, 0);
        AppendPbBytes(out, 3, desc);
    }

    AppendPbStr(out, 6, "Frametop 0.3.0");
    AppendPbVarint(out, 8, 0);
    const auto f9 = BuildAppleAudioF9Tiers();
    out.insert(out.end(), f9.begin(), f9.end());
    AppendPbVarint(out, 13, timestamp);
    AppendPbVarint(out, 14, 2);
    AppendPbVarint(out, 16, 0);
    AppendPbVarint(out, 18, 1);
    return out;
}

inline bool ExtractOfferSsrc(const std::vector<uint8_t> &bplist, bool is_video, uint32_t *out_ssrc) {
    std::vector<uint8_t> zblob, blob;
    if (!ParseBplistMediaBlob(bplist.data(), bplist.size(), &zblob)) return false;
    if (!ZlibDecompress(zblob.data(), zblob.size(), &blob)) return false;
    const uint32_t target = is_video ? 5u : 3u;
    size_t pos = 0;
    while (pos < blob.size()) {
        uint64_t tag = 0;
        if (!ReadVarint(blob.data(), blob.size(), &pos, &tag)) break;
        const uint32_t fn = uint32_t(tag >> 3);
        const uint32_t wt = uint32_t(tag & 7);
        if (wt == 0) {
            uint64_t dummy = 0;
            if (!ReadVarint(blob.data(), blob.size(), &pos, &dummy)) break;
        } else if (wt == 2) {
            uint64_t len = 0;
            if (!ReadVarint(blob.data(), blob.size(), &pos, &len) || len > blob.size() - pos) break;
            if (fn == target) {
                size_t ipos = pos;
                uint64_t itag = 0, ssrc = 0;
                if (ReadVarint(blob.data(), pos + size_t(len), &ipos, &itag) &&
                    (itag & 7) == 0 && (itag >> 3) == 1 &&
                    ReadVarint(blob.data(), pos + size_t(len), &ipos, &ssrc)) {
                    *out_ssrc = uint32_t(ssrc & 0xffffffffu);
                    return true;
                }
            }
            pos += size_t(len);
        } else if (wt == 1) {
            if (blob.size() - pos < 8) break;
            pos += 8;
        } else if (wt == 5) {
            if (blob.size() - pos < 4) break;
            pos += 4;
        } else {
            break;
        }
    }
    return false;
}

inline bool CreateNegotiatorOffers(const OfferConfig &cfg, BuiltOffers *out) {
    uint8_t raw_ids[8];
    if (!RandomBytes(raw_ids, sizeof raw_ids)) return false;
    const uint32_t vid_sid = ReadBe32(raw_ids);
    const uint32_t aud_sid = ReadBe32(raw_ids + 4);
    const uint32_t ts = uint32_t(uint64_t(std::time(nullptr)) & 0xffffffffu);

    uint8_t uuid_v[16], uuid_a[16];
    MakeUuidV4(uuid_v);
    MakeUuidV4(uuid_a);

    std::vector<uint8_t> zv, za;
    if (!ZlibCompress(BuildMediaBlob(7, vid_sid, ts, cfg), &zv)) return false;
    if (!ZlibCompress(BuildMediaBlob(8, aud_sid, ts, cfg), &za)) return false;

    out->video_bplist = BuildOfferBplist(7, zv, FormatUuidUpper(uuid_v));
    out->audio_bplist = BuildOfferBplist(8, za, FormatUuidUpper(uuid_a));
    out->video_ssrc = vid_sid;
    out->audio_ssrc = aud_sid;
    return true;
}

struct NegotiatedCanvas {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t tiles = 0;
    uint32_t ltrp_enabled = 0;
};

inline bool ParseNegotiatedVideoProtobuf(const std::vector<uint8_t> &dec, NegotiatedCanvas *out) {
    uint32_t cw = 0, ch = 0, ct = 0, ltr = 0;
    size_t pos = 0;
    while (pos < dec.size()) {
        uint64_t tag = 0;
        if (!ReadVarint(dec.data(), dec.size(), &pos, &tag)) break;
        const uint32_t fn = uint32_t(tag >> 3);
        const uint32_t wt = uint32_t(tag & 7);
        if (wt == 0) {
            uint64_t dummy = 0;
            if (!ReadVarint(dec.data(), dec.size(), &pos, &dummy)) break;
        } else if (wt == 2) {
            uint64_t slen = 0;
            if (!ReadVarint(dec.data(), dec.size(), &pos, &slen) || slen > dec.size() - pos) break;
            if (fn == 5) {
                size_t sp = pos;
                const size_t send = pos + size_t(slen);
                while (sp < send) {
                    uint64_t stag = 0;
                    if (!ReadVarint(dec.data(), send, &sp, &stag)) break;
                    const uint32_t sfn = uint32_t(stag >> 3);
                    const uint32_t swt = uint32_t(stag & 7);
                    if (swt == 0) {
                        uint64_t v = 0;
                        if (!ReadVarint(dec.data(), send, &sp, &v)) break;
                        if (sfn == 4) cw = uint32_t(v);
                        else if (sfn == 5) ch = uint32_t(v);
                        else if (sfn == 6) ct = uint32_t(v);
                        else if (sfn == 7) ltr = uint32_t(v);
                    } else if (swt == 2) {
                        uint64_t ilen = 0;
                        if (!ReadVarint(dec.data(), send, &sp, &ilen) || ilen > send - sp) break;
                        sp += size_t(ilen);
                    } else if (swt == 1) {
                        if (send - sp < 8) break;
                        sp += 8;
                    } else if (swt == 5) {
                        if (send - sp < 4) break;
                        sp += 4;
                    } else {
                        break;
                    }
                }
            }
            pos += size_t(slen);
        } else if (wt == 1) {
            if (dec.size() - pos < 8) break;
            pos += 8;
        } else if (wt == 5) {
            if (dec.size() - pos < 4) break;
            pos += 4;
        } else {
            break;
        }
    }
    if (cw > 0 && ch > 0) {
        out->width = cw;
        out->height = ch;
        out->tiles = ct ? ct : 1;
        out->ltrp_enabled = ltr;
        return true;
    }
    return false;
}

inline bool ExtractCanvasDims(const std::vector<uint8_t> &answer_msg, NegotiatedCanvas *out) {
    if (answer_msg.empty() || answer_msg[0] != 0x00) return false;
    // 1. macOS 14/15: AVCMediaStreamNegotiator.answer wraps the zlib protobuf in a bplist00 dict.
    for (size_t idx = 0; idx + 40 <= answer_msg.size(); ++idx) {
        if (std::memcmp(answer_msg.data() + idx, "bplist00", 8) != 0) continue;
        for (size_t end = idx + 40; end <= answer_msg.size(); ++end) {
            // Fast trailer check before full bplist parse: off_table + num_objs*off_sz == bplist_len - 32.
            const uint8_t *tr = answer_msg.data() + end - 32;
            const uint8_t off_sz = tr[6];
            const uint64_t num_objs = ReadBe64(tr + 8);
            const uint64_t off_tbl = ReadBe64(tr + 24);
            const size_t blen = end - idx;
            if (off_sz < 1 || off_sz > 8 || num_objs == 0 || num_objs > 4096) continue;
            if (off_tbl + num_objs * off_sz != blen - 32) continue;

            std::vector<uint8_t> zblob, dec;
            if (!ParseBplistMediaBlob(answer_msg.data() + idx, blen, &zblob)) continue;
            if (!ZlibDecompress(zblob.data(), zblob.size(), &dec)) continue;
            if (ParseNegotiatedVideoProtobuf(dec, out)) return true;
        }
    }
    // 2. macOS 26/27+: AVCMediaStreamNegotiator.answer emits a raw RFC 1950 zlib stream (e.g. 0x78 0xda)
    //    directly without a bplist00 wrapper. ZlibDecompress stops cleanly at Z_STREAM_END + Adler32.
    for (size_t idx = 4; idx + 16 <= answer_msg.size(); ++idx) {
        const uint8_t cmf = answer_msg[idx];
        const uint8_t flg = answer_msg[idx + 1];
        if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || (flg & 0x20) != 0) continue;
        if (((uint16_t(cmf) << 8) | uint16_t(flg)) % 31 != 0) continue;
        std::vector<uint8_t> dec;
        if (ZlibDecompress(answer_msg.data() + idx, answer_msg.size() - idx, &dec) &&
            ParseNegotiatedVideoProtobuf(dec, out)) {
            return true;
        }
    }
    return false;
}

struct NegotiationKeys {
    std::array<uint8_t, kSrtpKeyBlobLen> audio_key_v{};
    std::array<uint8_t, kSrtpKeyBlobLen> audio_key_s{};
    std::array<uint8_t, kSrtpKeyBlobLen> video_key_v{};
    std::array<uint8_t, kSrtpKeyBlobLen> video_key_s{};

    static bool Generate(NegotiationKeys *k) {
        return RandomBytes(k->audio_key_v.data(), kSrtpKeyBlobLen) &&
               RandomBytes(k->audio_key_s.data(), kSrtpKeyBlobLen) &&
               RandomBytes(k->video_key_v.data(), kSrtpKeyBlobLen) &&
               RandomBytes(k->video_key_s.data(), kSrtpKeyBlobLen);
    }
};

inline std::vector<uint8_t> Build0x1cMessage(const std::vector<uint8_t> &audio_offer,
                                             const std::vector<uint8_t> &video_offer,
                                             const NegotiationKeys &keys,
                                             bool bake_cursor = true,
                                             bool supports_60fps = true) {
    const uint16_t as = uint16_t(audio_offer.size());
    const uint16_t vs = uint16_t(video_offer.size());
    const uint16_t ms = uint16_t(as + vs + 0xd8);
    std::vector<uint8_t> buf(size_t(ms) + 4, 0);
    buf[0] = 0x1c;
    WriteBe16(&buf[2], ms);
    WriteBe16(&buf[4], 3);
    // Big-endian config_flags: bit 0 = 60fps (0 = 30fps), bit 1 = media v2, bit 2 = strip cursor.
    const uint32_t flags = (bake_cursor ? 2u : 6u) | (supports_60fps ? 1u : 0u);
    WriteBe32(&buf[6], flags);
    WriteBe16(&buf[10], as);
    WriteBe16(&buf[12], vs);

    uint8_t call_uuid[16];
    MakeUuidV4(call_uuid);
    std::memcpy(&buf[0x14], call_uuid, 16);
    std::memcpy(&buf[0x24], keys.audio_key_v.data(), kSrtpKeyBlobLen);
    std::memcpy(&buf[0x52], keys.audio_key_s.data(), kSrtpKeyBlobLen);
    std::memcpy(&buf[0x80], audio_offer.data(), as);
    const size_t vo = 0x80 + size_t(as);
    std::memcpy(&buf[vo], keys.video_key_v.data(), kSrtpKeyBlobLen);
    std::memcpy(&buf[vo + 0x2e], keys.video_key_s.data(), kSrtpKeyBlobLen);
    std::memcpy(&buf[vo + 0x5c], video_offer.data(), vs);
    return buf;
}

// ── enc1103 Control Record Cipher (AES-128-CBC + Plain SHA-1 Counter Tag) ────

class StreamCipher {
public:
    bool InitFromRekey0x44f(const uint8_t rekey36[36], const uint8_t wrap_key16[16]) {
        if (!Aes128EcbCrypt(false, wrap_key16, rekey36 + 4, 16, cbc_key_.data())) return false;
        if (!Aes128EcbCrypt(false, wrap_key16, rekey36 + 20, 16, cbc_iv_.data())) return false;
        tx_ctx_.reset(EVP_CIPHER_CTX_new());
        rx_ctx_.reset(EVP_CIPHER_CTX_new());
        if (!tx_ctx_ || !rx_ctx_) return false;
        if (EVP_EncryptInit_ex(tx_ctx_.get(), EVP_aes_128_cbc(), nullptr, cbc_key_.data(), cbc_iv_.data()) != 1)
            return false;
        if (EVP_DecryptInit_ex(rx_ctx_.get(), EVP_aes_128_cbc(), nullptr, cbc_key_.data(), cbc_iv_.data()) != 1)
            return false;
        EVP_CIPHER_CTX_set_padding(tx_ctx_.get(), 0);
        EVP_CIPHER_CTX_set_padding(rx_ctx_.get(), 0);
        tx_counter_ = 0;
        rx_counter_ = 0;
        return true;
    }

    const std::array<uint8_t, 16> &cbc_key() const { return cbc_key_; }

    bool EncryptMessage(const uint8_t *msg, size_t msg_len, std::vector<uint8_t> *wire_out) {
        if (!tx_ctx_ || msg_len > 0xffff - 38) return false;
        const size_t pre_mac = 2 + msg_len;
        const size_t pad_len = (16 - ((pre_mac + 20) % 16)) % 16;
        const size_t plain_len = pre_mac + pad_len + 20;

        std::vector<uint8_t> plain(plain_len, 0);
        WriteBe16(plain.data(), uint16_t(msg_len));
        if (msg_len > 0) std::memcpy(plain.data() + 2, msg, msg_len);
        const auto tag = Sha1SeqAndBody(tx_counter_, plain.data(), pre_mac + pad_len);
        std::memcpy(plain.data() + pre_mac + pad_len, tag.data(), 20);

        wire_out->resize(2 + plain_len);
        WriteBe16(wire_out->data(), uint16_t(plain_len));
        int out_len = 0;
        if (EVP_EncryptUpdate(tx_ctx_.get(), wire_out->data() + 2, &out_len, plain.data(), int(plain_len)) != 1 ||
            size_t(out_len) != plain_len) {
            return false;
        }
        ++tx_counter_;
        return true;
    }

    bool EncryptMessage(const std::vector<uint8_t> &msg, std::vector<uint8_t> *wire_out) {
        return EncryptMessage(msg.data(), msg.size(), wire_out);
    }

    // Decrypts one record given ciphertext without the leading u16_be length.
    bool DecryptRecordBody(const uint8_t *cipher, size_t cipher_len, std::vector<uint8_t> *msg_out) {
        if (!rx_ctx_ || cipher_len < 32 || (cipher_len % 16) != 0) return false;
        std::vector<uint8_t> plain(cipher_len);
        int out_len = 0;
        if (EVP_DecryptUpdate(rx_ctx_.get(), plain.data(), &out_len, cipher, int(cipher_len)) != 1 ||
            size_t(out_len) != cipher_len) {
            return false;
        }
        const uint16_t msg_len = ReadBe16(plain.data());
        if (size_t(2) + msg_len + 20 > cipher_len) return false;

        const uint8_t *wire_tag = plain.data() + cipher_len - 20;
        bool matched = false;
        for (uint32_t delta = 0; delta <= 6; ++delta) {
            const auto expect = Sha1SeqAndBody(rx_counter_ + delta, plain.data(), cipher_len - 20);
            if (CRYPTO_memcmp(expect.data(), wire_tag, 20) == 0) {
                rx_counter_ += delta + 1;
                matched = true;
                break;
            }
        }
        if (!matched) return false;
        msg_out->assign(plain.data() + 2, plain.data() + 2 + msg_len);
        return true;
    }

private:
    std::array<uint8_t, 16> cbc_key_{};
    std::array<uint8_t, 16> cbc_iv_{};
    UniqueCipherCtx tx_ctx_;
    UniqueCipherCtx rx_ctx_;
    uint32_t tx_counter_ = 0;
    uint32_t rx_counter_ = 0;
};

// ── Apple RFB Handshake & Control Messages ───────────────────────────────────

inline std::vector<uint8_t> BuildViewerInfo() {
    std::vector<uint8_t> mask(32, 0);
    mask[0] = 0xb0;
    mask[2] = 0x0c;
    mask[3] = 0x03;
    mask[4] = 0x90;
    mask[10] = 0x40;

    std::vector<uint8_t> out;
    out.push_back(0x21);
    out.push_back(0x00);
    AppendBe16(out, 0x3e);  // 62 bytes after msgSize
    AppendBe16(out, 1);     // msgVersion
    AppendBe32(out, 2);     // app_id = 2
    AppendBe32(out, 6);     // app_ver = 6.1.0
    AppendBe32(out, 1);
    AppendBe32(out, 0);
    AppendBe32(out, 15);    // os_ver = 15.3.0
    AppendBe32(out, 3);
    AppendBe32(out, 0);
    out.insert(out.end(), mask.begin(), mask.end());
    return out;
}

inline std::vector<uint8_t> Build0x12Followup() {
    return {0x12, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01};
}

inline std::vector<uint8_t> BuildPostEncryptionToggle() {
    return {0x12, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00};
}

inline std::vector<uint8_t> BuildSetEncodings() {
    static constexpr int32_t kEncs[] = {
        1010, 1011, 1002, 6, 16, 1104, 1100, -223, 1101, 1105, 1107, 1109, 1110,
    };
    constexpr uint16_t n = sizeof(kEncs) / sizeof(kEncs[0]);
    std::vector<uint8_t> out = {0x02, 0x00};
    AppendBe16(out, n);
    for (int32_t e : kEncs) AppendBe32(out, uint32_t(e));
    return out;
}

inline std::vector<uint8_t> BuildFbuRequest(bool incremental, uint16_t x = 0, uint16_t y = 0,
                                            uint16_t w = 0xffff, uint16_t h = 0xffff) {
    std::vector<uint8_t> out = {0x03, uint8_t(incremental ? 1 : 0)};
    AppendBe16(out, x);
    AppendBe16(out, y);
    AppendBe16(out, w);
    AppendBe16(out, h);
    return out;
}

inline std::vector<uint8_t> BuildVirtualDisplay0x1d(uint32_t logical_w, uint32_t logical_h,
                                                    double hidpi_scale = 2.0, bool hdr = false,
                                                    const std::string &name = "Frametop Virtual Display",
                                                    double refresh_hz = 60.0) {
    constexpr uint16_t kModeCount = 5;
    constexpr uint16_t kDiSize = 0x9c + 28 * kModeCount;  // 296 bytes
    std::vector<uint8_t> di(kDiSize, 0);
    WriteBe16(&di[0x00], kDiSize);
    const size_t nlen = std::min<size_t>(119, name.size());
    std::memcpy(&di[0x02], name.data(), nlen);
    const uint32_t target_mw = std::clamp<uint32_t>(
        uint32_t(std::lround(logical_w * hidpi_scale)) & ~1u, 1280, 4096);
    const uint32_t target_mh = std::clamp<uint32_t>(
        uint32_t(std::lround(logical_h * hidpi_scale)) & ~1u, 720, 2304);
    const uint32_t max_w = std::max<uint32_t>(3840, target_mw);
    const uint32_t max_h = std::max<uint32_t>(2160, target_mh);

    WriteBe32(&di[0x7a], 1);      // DYNAMIC_RESOLUTION
    WriteBe32(&di[0x7e], 4);      // display_type = 4 (virtual display)
    WriteF32Be(&di[0x82], 369.45f);
    WriteF32Be(&di[0x86], 207.82f);
    WriteBe32(&di[0x8a], max_w);  // max_width (up to Apple Silicon H.264 4096-px ceiling)
    WriteBe32(&di[0x8e], max_h);  // max_height
    WriteBe16(&di[0x92], 0);      // current_mode_index
    WriteBe16(&di[0x94], 0);      // preferred_mode_index
    WriteBe32(&di[0x96], 7);      // rotations / native 0x1d marker
    WriteBe16(&di[0x9a], kModeCount);

    static constexpr double kScales[kModeCount] = {1.0, 0.85, 0.75, 0.67, 0.5};
    const uint32_t mode_flags = hdr ? 1u : 0u;
    const double hz = std::clamp(refresh_hz, 15.0, 120.0);
    for (uint16_t i = 0; i < kModeCount; ++i) {
        const uint32_t msw = std::max<uint32_t>(640, uint32_t(std::lround(logical_w * kScales[i])) & ~1u);
        const uint32_t msh = std::max<uint32_t>(360, uint32_t(std::lround(logical_h * kScales[i])) & ~1u);
        const uint32_t mw = std::min<uint32_t>(max_w, uint32_t(std::lround(msw * hidpi_scale)) & ~1u);
        const uint32_t mh = std::min<uint32_t>(max_h, uint32_t(std::lround(msh * hidpi_scale)) & ~1u);
        const size_t m = 0x9c + 28 * size_t(i);
        WriteBe32(&di[m + 0x00], mw);
        WriteBe32(&di[m + 0x04], mh);
        WriteBe32(&di[m + 0x08], msw);
        WriteBe32(&di[m + 0x0c], msh);
        WriteF64Be(&di[m + 0x10], hz);
        WriteBe32(&di[m + 0x18], mode_flags);
    }

    const uint16_t msg_size = 8 + kDiSize;  // 304 bytes -> 308 total
    std::vector<uint8_t> out = {0x1d, 0x00};
    AppendBe16(out, msg_size);
    AppendBe16(out, 1);
    AppendBe16(out, 1);
    AppendBe32(out, 0);
    out.insert(out.end(), di.begin(), di.end());
    return out;
}

struct DisplayLayout0x451 {
    uint16_t scaled_w = 0;
    uint16_t scaled_h = 0;
    uint16_t backing_w = 0;
    uint16_t backing_h = 0;
    uint16_t display_count = 0;
};

// Parses payload AFTER the u16 payload_len prefix of an encoding 0x451 rectangle.
inline bool ParseDisplayLayout0x451(const uint8_t *payload, size_t len, DisplayLayout0x451 *out) {
    if (len < 20) return false;
    out->scaled_w = ReadBe16(payload + 2);
    out->scaled_h = ReadBe16(payload + 4);
    out->backing_w = ReadBe16(payload + 6);
    out->backing_h = ReadBe16(payload + 8);
    out->display_count = ReadBe16(payload + 18);
    return out->backing_w > 0 && out->backing_h > 0;
}

}  // namespace ft_apple_hp
