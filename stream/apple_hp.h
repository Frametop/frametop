// Clean-room Apple macOS High Performance Screen Sharing client core (MIT).
// Speaks screensharingd's RFB 003.889 + RSA1/SRP-6a + enc1103 control channel (TCP 5900)
// and AVCMediaStreamNegotiator + RFC 3711 SRTP H.264 4:2:0 media stream (UDP 5900/5901).
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ft_apple_hp {

constexpr size_t kSrtpKeyBlobLen = 46;
constexpr size_t kSrtpMasterKeyLen = 32;
constexpr size_t kSrtpMasterSaltLen = 14;
constexpr size_t kSrtpTagLen = 10;
constexpr size_t kSrpModulusLen = 512;
constexpr size_t kSrpHashLen = 64;
constexpr uint8_t kAppleAvcConfigMarker = 0x92;

inline uint16_t ReadBe16(const uint8_t *p) {
    return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

inline uint32_t ReadBe32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline uint64_t ReadBe64(const uint8_t *p) {
    return (uint64_t(ReadBe32(p)) << 32) | uint64_t(ReadBe32(p + 4));
}

inline void WriteBe16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v & 0xff);
}

inline void WriteBe32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t((v >> 16) & 0xff);
    p[2] = uint8_t((v >> 8) & 0xff);
    p[3] = uint8_t(v & 0xff);
}

inline void WriteBe64(uint8_t *p, uint64_t v) {
    WriteBe32(p, uint32_t(v >> 32));
    WriteBe32(p + 4, uint32_t(v & 0xffffffffu));
}

inline void AppendBe16(std::vector<uint8_t> &b, uint16_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v & 0xff));
}

inline void AppendBe32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(uint8_t(v >> 24));
    b.push_back(uint8_t((v >> 16) & 0xff));
    b.push_back(uint8_t((v >> 8) & 0xff));
    b.push_back(uint8_t(v & 0xff));
}

inline void AppendBe64(std::vector<uint8_t> &b, uint64_t v) {
    AppendBe32(b, uint32_t(v >> 32));
    AppendBe32(b, uint32_t(v & 0xffffffffu));
}

inline void WriteF32Be(uint8_t *p, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    WriteBe32(p, bits);
}

inline void WriteF64Be(uint8_t *p, double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, 8);
    WriteBe64(p, bits);
}

inline int64_t NowNs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

// ── OpenSSL 3.x / 4.x RAII & Crypto Primitives ───────────────────────────────

struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX *p) const { EVP_MD_CTX_free(p); }
};
struct EvpCipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX *p) const { EVP_CIPHER_CTX_free(p); }
};
struct EvpPkeyDeleter {
    void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
};
struct EvpPkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX *p) const { EVP_PKEY_CTX_free(p); }
};
struct BnDeleter {
    void operator()(BIGNUM *p) const { BN_clear_free(p); }
};
struct BnCtxDeleter {
    void operator()(BN_CTX *p) const { BN_CTX_free(p); }
};

using UniqueMdCtx = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;
using UniqueCipherCtx = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;
using UniquePkey = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using UniquePkeyCtx = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;
using UniqueBn = std::unique_ptr<BIGNUM, BnDeleter>;
using UniqueBnCtx = std::unique_ptr<BN_CTX, BnCtxDeleter>;

inline bool RandomBytes(uint8_t *out, size_t len) {
    return len == 0 || RAND_bytes(out, int(len)) == 1;
}

inline std::vector<uint8_t> RandomVec(size_t len) {
    std::vector<uint8_t> v(len);
    if (!RandomBytes(v.data(), len)) v.clear();
    return v;
}

inline std::string FormatUuidUpper(const uint8_t uuid[16]) {
    char buf[37];
    std::snprintf(buf, sizeof buf,
                  "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
                  uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return buf;
}

inline void MakeUuidV4(uint8_t uuid[16]) {
    RandomBytes(uuid, 16);
    uuid[6] = uint8_t((uuid[6] & 0x0f) | 0x40);
    uuid[8] = uint8_t((uuid[8] & 0x3f) | 0x80);
}

inline std::vector<uint8_t> DigestConcat(const EVP_MD *md,
                                         const std::vector<std::pair<const uint8_t *, size_t>> &parts) {
    UniqueMdCtx ctx(EVP_MD_CTX_new());
    std::vector<uint8_t> out(size_t(EVP_MD_get_size(md)));
    unsigned int out_len = 0;
    if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) return {};
    for (const auto &p : parts) {
        if (p.second > 0 && EVP_DigestUpdate(ctx.get(), p.first, p.second) != 1) return {};
    }
    if (EVP_DigestFinal_ex(ctx.get(), out.data(), &out_len) != 1) return {};
    out.resize(out_len);
    return out;
}

inline std::array<uint8_t, 20> Sha1SeqAndBody(uint32_t seq_be_val, const uint8_t *data, size_t len) {
    uint8_t seq_be[4];
    WriteBe32(seq_be, seq_be_val);
    auto v = DigestConcat(EVP_sha1(), {{seq_be, 4}, {data, len}});
    std::array<uint8_t, 20> res{};
    if (v.size() == 20) std::memcpy(res.data(), v.data(), 20);
    return res;
}

inline std::array<uint8_t, 32> Sha256(const uint8_t *data, size_t len) {
    auto v = DigestConcat(EVP_sha256(), {{data, len}});
    std::array<uint8_t, 32> res{};
    if (v.size() == 32) std::memcpy(res.data(), v.data(), 32);
    return res;
}

inline std::array<uint8_t, 64> Sha512Parts(const std::vector<std::pair<const uint8_t *, size_t>> &parts) {
    auto v = DigestConcat(EVP_sha512(), parts);
    std::array<uint8_t, 64> res{};
    if (v.size() == 64) std::memcpy(res.data(), v.data(), 64);
    return res;
}

inline bool HmacSha1(const uint8_t *key, size_t key_len,
                     const uint8_t *d1, size_t len1,
                     const uint8_t *d2, size_t len2,
                     uint8_t out20[20]) {
    EVP_MAC *mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (!mac) return false;
    EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    if (!ctx) return false;
    char digest_name[] = "SHA1";
    OSSL_PARAM params[2] = {
        OSSL_PARAM_construct_utf8_string("digest", digest_name, 0),
        OSSL_PARAM_construct_end(),
    };
    size_t out_len = 0;
    const bool ok = EVP_MAC_init(ctx, key, key_len, params) == 1 &&
                    (len1 == 0 || EVP_MAC_update(ctx, d1, len1) == 1) &&
                    (len2 == 0 || EVP_MAC_update(ctx, d2, len2) == 1) &&
                    EVP_MAC_final(ctx, out20, &out_len, 20) == 1 &&
                    out_len == 20;
    EVP_MAC_CTX_free(ctx);
    return ok;
}

inline bool Aes128EcbCrypt(bool encrypt, const uint8_t key16[16], const uint8_t *in, size_t len, uint8_t *out) {
    if (len % 16 != 0) return false;
    UniqueCipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;
    if (EVP_CipherInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key16, nullptr, encrypt ? 1 : 0) != 1) return false;
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    int out_len = 0, tail_len = 0;
    return EVP_CipherUpdate(ctx.get(), out, &out_len, in, int(len)) == 1 &&
           EVP_CipherFinal_ex(ctx.get(), out + out_len, &tail_len) == 1 &&
           size_t(out_len + tail_len) == len;
}

inline bool Aes256CtrCrypt(const uint8_t key32[32], const uint8_t iv16[16],
                           const uint8_t *in, size_t len, uint8_t *out) {
    UniqueCipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_ctr(), nullptr, key32, iv16) != 1) return false;
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    int out_len = 0, tail_len = 0;
    return EVP_EncryptUpdate(ctx.get(), out, &out_len, in, int(len)) == 1 &&
           EVP_EncryptFinal_ex(ctx.get(), out + out_len, &tail_len) == 1 &&
           size_t(out_len + tail_len) == len;
}

inline bool Pbkdf2HmacSha512(const std::string &password, const std::vector<uint8_t> &salt,
                             uint64_t iterations, uint8_t out128[128]) {
    if (iterations == 0 || iterations > 1'000'000) return false;
    return PKCS5_PBKDF2_HMAC(password.data(), int(password.size()),
                             salt.data(), int(salt.size()),
                             int(iterations), EVP_sha512(), 128, out128) == 1;
}

inline UniquePkey LoadDerPublicKey(const uint8_t *der, size_t len) {
    const unsigned char *p = der;
    return UniquePkey(d2i_PUBKEY(nullptr, &p, long(len)));
}

inline bool RsaPkcs1Encrypt(EVP_PKEY *pub, const uint8_t *plain, size_t plain_len, std::vector<uint8_t> *out) {
    if (!pub) return false;
    UniquePkeyCtx ctx(EVP_PKEY_CTX_new(pub, nullptr));
    if (!ctx || EVP_PKEY_encrypt_init(ctx.get()) <= 0) return false;
    if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING) <= 0) return false;
    size_t out_len = 0;
    if (EVP_PKEY_encrypt(ctx.get(), nullptr, &out_len, plain, plain_len) <= 0) return false;
    out->resize(out_len);
    if (EVP_PKEY_encrypt(ctx.get(), out->data(), &out_len, plain, plain_len) <= 0) return false;
    out->resize(out_len);
    return true;
}

inline bool ZlibCompress(const std::vector<uint8_t> &in, std::vector<uint8_t> *out) {
    uLongf cap = compressBound(uLong(in.size()));
    out->resize(cap);
    if (compress2(out->data(), &cap, in.data(), uLong(in.size()), Z_DEFAULT_COMPRESSION) != Z_OK) return false;
    out->resize(cap);
    return true;
}

inline bool ZlibDecompress(const uint8_t *in, size_t in_len, std::vector<uint8_t> *out, size_t max_out = 262144) {
    z_stream zs{};
    zs.next_in = const_cast<Bytef *>(in);
    zs.avail_in = uInt(in_len);
    if (inflateInit(&zs) != Z_OK) return false;
    out->clear();
    uint8_t chunk[4096];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = chunk;
        zs.avail_out = sizeof chunk;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        const size_t have = sizeof(chunk) - zs.avail_out;
        if (out->size() + have > max_out) {
            inflateEnd(&zs);
            return false;
        }
        out->insert(out->end(), chunk, chunk + have);
    }
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

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
    if (off_tbl < 8 || off_tbl + num_objs * off_sz > len - 32) return false;

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

// ── RFB Client Input Builders & Linux Evdev -> Apple Keysym Translator ───────

// Synthetic internal keysym tag for raw macOS virtual keycodes (CGKeyCode 0x00..0xff) that have
// no X11 keysym in ScreensharingAgent's KeyMapDictionary (such as kVK_Function / Globe = 0x3f),
// dispatched on the wire via Apple's 0x10 EncryptedEventMessage instead of RFB 0x04 KeyEvent.
inline constexpr uint32_t kAppleMacKeycodeTag = 0x02000000u;
inline constexpr uint16_t kMacVkFunction = 0x003fu;        // kVK_Function (macOS Globe / Fn)
inline constexpr uint16_t kAppleKeyboardTypeAnsi = 0x003du; // Apple Silicon / ANSI keyboardType 61
inline constexpr uint32_t kAppleKeysymGlobe = kAppleMacKeycodeTag | kMacVkFunction;

inline bool IsAppleMacKeycodeKeysym(uint32_t ks, uint16_t *out_vk = nullptr) {
    if ((ks & 0xffffff00u) != kAppleMacKeycodeTag) return false;
    if (out_vk) *out_vk = uint16_t(ks & 0xffu);
    return true;
}

inline std::vector<uint8_t> BuildKeyEvent(bool down, uint32_t keysym) {
    std::vector<uint8_t> out = {0x04, uint8_t(down ? 1 : 0), 0x00, 0x00};
    AppendBe32(out, keysym);
    return out;
}

// Apple RFB client message 0x10 (HandleEncryptedEventMessage, 18 bytes):
//   [0]      0x10  opcode
//   [1]      0x03  flags (bit 0 | bit 1 -> ProcessKey sets use_keycode = 1 across viewer versions)
//   [2..17]  AES-128-ECB(cbc_key16, plain16), decrypted by screensharingd's [viewer+0x5f8] cryptor:
//              plain[0]      = 0xff             (keyboard event marker)
//              plain[1]      = down ? 1 : 0     (key-down state -> PostKeyEvent down)
//              plain[2..5]   = be32(keysym)     (0 when posting a raw macOS CGKeyCode)
//              plain[6..9]   = 0x00000000
//              plain[10]     = 0x00             (standalone key event; no coalesced pointer event)
//              plain[11]     = 0x00
//              plain[12..13] = be16(kb_type)    (0x003d = Apple keyboard type 61)
//              plain[14..15] = be16(mac_keycode)(e.g. 0x003f = kVK_Function / Globe)
inline bool BuildAppleKeycodeEvent0x10(const uint8_t ecb_key16[16], bool down, uint16_t mac_keycode,
                                       std::vector<uint8_t> *out, uint32_t keysym = 0,
                                       uint16_t keyboard_type = kAppleKeyboardTypeAnsi) {
    if (!out) return false;
    uint8_t plain[16]{};
    plain[0] = 0xff;
    plain[1] = uint8_t(down ? 1 : 0);
    WriteBe32(&plain[2], keysym);
    plain[10] = 0x00;
    WriteBe16(&plain[12], keyboard_type);
    WriteBe16(&plain[14], mac_keycode);
    out->assign(18, 0);
    (*out)[0] = 0x10;
    (*out)[1] = 0x03;
    return Aes128EcbCrypt(true, ecb_key16, plain, 16, out->data() + 2);
}

inline bool ParseAppleKeycodeEvent0x10(const uint8_t ecb_key16[16], const uint8_t *msg, size_t len,
                                       bool *out_down, uint16_t *out_keycode,
                                       uint16_t *out_kb_type = nullptr, uint32_t *out_keysym = nullptr) {
    if (!msg || len != 18 || msg[0] != 0x10 || (msg[1] & 0x01) == 0) return false;
    uint8_t plain[16]{};
    if (!Aes128EcbCrypt(false, ecb_key16, msg + 2, 16, plain)) return false;
    if (plain[0] != 0xff || (plain[10] != 0x00 && plain[10] != 0xff)) return false;
    if (out_down) *out_down = (plain[1] != 0);
    if (out_keysym) *out_keysym = ReadBe32(&plain[2]);
    if (out_kb_type) *out_kb_type = ReadBe16(&plain[12]);
    if (out_keycode) *out_keycode = ReadBe16(&plain[14]);
    return true;
}

inline std::vector<uint8_t> BuildPointerEvent(uint8_t button_mask, uint16_t x, uint16_t y) {
    std::vector<uint8_t> out = {0x05, button_mask};
    AppendBe16(out, x);
    AppendBe16(out, y);
    return out;
}

inline uint16_t ScalePointerCoord(double stream_px, uint32_t stream_dim, uint16_t backing_dim) {
    if (backing_dim == 0) return 0;
    const long max_coord = std::min<long>(long(backing_dim) - 1, 32767);
    if (stream_dim == 0) {
        return uint16_t(std::clamp<long>(std::lround(stream_px), 0, max_coord));
    }
    const double clamped = std::clamp(stream_px, 0.0, double(stream_dim - 1));
    if (stream_dim == backing_dim) {
        return uint16_t(std::clamp<long>(std::lround(clamped), 0, max_coord));
    }
    const double denom = double(stream_dim > 1 ? stream_dim - 1 : 1);
    const double scaled = (clamped * double(backing_dim - 1)) / denom;
    return uint16_t(std::clamp<long>(std::lround(scaled), 0, max_coord));
}

// Because BuildViewerInfo() identifies us as an Apple viewer (viewer->appleViewerFlag = 1),
// screensharingd's HandlePointerEventMessage skips the generic-VNC 0x02<->0x04 button swap and
// passes button bits directly to ScreensharingAgent's PostMouseEventIntoSession in CGMouseButton
// order: bit 0 (0x01) = Left (0), bit 1 (0x02) = Right (1), bit 2 (0x04) = Middle/Center (2).
inline uint8_t MouseButtonBit(uint32_t linux_btn) {
    switch (linux_btn) {
        case 0x110: return 0x01;  // BTN_LEFT   -> kCGMouseButtonLeft
        case 0x111: return 0x02;  // BTN_RIGHT  -> kCGMouseButtonRight
        case 0x112: return 0x04;  // BTN_MIDDLE -> kCGMouseButtonCenter
        default: return 0x00;
    }
}

// ── Keyboard Layouts & Evdev-to-Apple Keysym Mapper ──────────────────────────

enum class KeyboardLayout {
    kQwerty,
    kDvorak,
    kDvorakQwertyCmd,
    kColemak,
};

inline const char *KeyboardLayoutName(KeyboardLayout layout) {
    switch (layout) {
        case KeyboardLayout::kQwerty:          return "qwerty";
        case KeyboardLayout::kDvorak:          return "dvorak";
        case KeyboardLayout::kDvorakQwertyCmd: return "dvorak-qwerty-cmd";
        case KeyboardLayout::kColemak:         return "colemak";
    }
    return "qwerty";
}

inline bool ParseKeyboardLayoutName(const std::string &raw, KeyboardLayout *out) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c >= 'A' && c <= 'Z') s.push_back(char(c - 'A' + 'a'));
        else if (c == '_') s.push_back('-');
        else if (c != ' ' && c != '\t') s.push_back(c);
    }
    KeyboardLayout parsed = KeyboardLayout::kQwerty;
    if (s == "qwerty" || s == "us" || s == "default") parsed = KeyboardLayout::kQwerty;
    else if (s == "dvorak") parsed = KeyboardLayout::kDvorak;
    else if (s == "dvorak-qwerty-cmd" || s == "dvorak-cmd") parsed = KeyboardLayout::kDvorakQwertyCmd;
    else if (s == "colemak") parsed = KeyboardLayout::kColemak;
    else return false;
    if (out) *out = parsed;
    return true;
}

// Parses a physical Linux evdev key name (e.g. "capslock", "leftctrl", "a".."z", "0".."9",
// "f1".."f20", "minus", "leftbrace", or "code:58"). When `allow_none` is true, "none"/"disabled"/"off"
// resolves to 0 so a remap target can suppress a key cleanly. Digit strings "0".."9" always name
// physical number-row keys (KEY_0..KEY_9), never raw evdev code 1 (KEY_ESC).
inline bool ParseEvdevKeyName(const std::string &raw, uint32_t *out_code, bool allow_none = false) {
    std::string low;
    low.reserve(raw.size());
    for (char c : raw) {
        if (c >= 'A' && c <= 'Z') low.push_back(char(c - 'A' + 'a'));
        else if (c != ' ' && c != '\t') low.push_back(c);
    }
    if (low.empty()) return false;

    auto parse_raw_prefix = [&](const char *prefix) -> int {
        const size_t plen = std::strlen(prefix);
        if (low.rfind(prefix, 0) != 0 || low.size() <= plen) return -1;
        int v = 0;
        for (size_t i = plen; i < low.size(); ++i) {
            if (low[i] < '0' || low[i] > '9') return -1;
            v = v * 10 + (low[i] - '0');
            if (v > 255) return -1;
        }
        return (v >= 1 && v <= 255 && v != 194) ? v : -1;
    };
    const int raw_num = std::max(parse_raw_prefix("code:"), parse_raw_prefix("evdev:"));
    if (raw_num > 0) {
        if (out_code) *out_code = uint32_t(raw_num);
        return true;
    }

    std::string s;
    if (low.size() > 1) {
        s.reserve(low.size());
        for (char c : low) {
            if (c != '-' && c != '_') s.push_back(c);
        }
    } else {
        s = low;
    }
    if (s.empty()) return false;

    if (allow_none && (s == "none" || s == "disabled" || s == "off")) {
        if (out_code) *out_code = 0;
        return true;
    }
    if (s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') {
        static constexpr uint8_t kAlphaEvdev[26] = {
            30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
            49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
        };
        if (out_code) *out_code = kAlphaEvdev[s[0] - 'a'];
        return true;
    }
    if (s.size() == 1 && s[0] >= '0' && s[0] <= '9') {
        if (out_code) *out_code = (s[0] == '0') ? 11u : uint32_t(2 + (s[0] - '1'));
        return true;
    }
    if (s.size() >= 2 && s.size() <= 3 && s[0] == 'f' && s[1] >= '1' && s[1] <= '9') {
        int fn = s[1] - '0';
        if (s.size() == 3) {
            if (s[2] < '0' || s[2] > '9') fn = 0;
            else fn = fn * 10 + (s[2] - '0');
        }
        if (fn >= 1 && fn <= 20) {
            uint32_t code = 0;
            if (fn <= 10) code = uint32_t(59 + (fn - 1));
            else if (fn == 11) code = 87;
            else if (fn == 12) code = 88;
            else code = uint32_t(183 + (fn - 13));
            if (out_code) *out_code = code;
            return true;
        }
    }

    struct NamedKey {
        const char *name;
        uint32_t code;
    };
    static constexpr NamedKey kNamed[] = {
        {"esc", 1}, {"escape", 1},
        {"minus", 12}, {"hyphen", 12},
        {"equal", 13}, {"equals", 13},
        {"backspace", 14}, {"bksp", 14},
        {"tab", 15},
        {"leftbrace", 26}, {"lbrace", 26}, {"leftbracket", 26}, {"lbracket", 26},
        {"rightbrace", 27}, {"rbrace", 27}, {"rightbracket", 27}, {"rbracket", 27},
        {"enter", 28}, {"return", 28},
        {"leftctrl", 29}, {"lctrl", 29}, {"ctrl", 29}, {"control", 29}, {"leftcontrol", 29},
        {"semicolon", 39},
        {"apostrophe", 40}, {"quote", 40},
        {"grave", 41}, {"backtick", 41}, {"tilde", 41},
        {"leftshift", 42}, {"lshift", 42}, {"shift", 42},
        {"backslash", 43},
        {"comma", 51},
        {"dot", 52}, {"period", 52},
        {"slash", 53},
        {"rightshift", 54}, {"rshift", 54},
        {"kpasterisk", 55}, {"kpmultiply", 55},
        {"leftalt", 56}, {"lalt", 56}, {"alt", 56}, {"opt", 56}, {"option", 56}, {"leftopt", 56}, {"leftoption", 56},
        {"space", 57}, {"spacebar", 57},
        {"capslock", 58}, {"caps", 58},
        {"numlock", 69},
        {"kp7", 71}, {"kp8", 72}, {"kp9", 73},
        {"kpminus", 74}, {"kpsubtract", 74},
        {"kp4", 75}, {"kp5", 76}, {"kp6", 77},
        {"kpplus", 78}, {"kpadd", 78},
        {"kp1", 79}, {"kp2", 80}, {"kp3", 81},
        {"kp0", 82}, {"kpdot", 83}, {"kpdecimal", 83},
        {"102nd", 86}, {"iso102nd", 86},
        {"kpenter", 96},
        {"rightctrl", 97}, {"rctrl", 97}, {"rightcontrol", 97},
        {"kpslash", 98}, {"kpdivide", 98},
        {"rightalt", 100}, {"ralt", 100}, {"altgr", 100}, {"ropt", 100}, {"roption", 100}, {"rightopt", 100}, {"rightoption", 100},
        {"home", 102}, {"up", 103}, {"pageup", 104}, {"pgup", 104},
        {"left", 105}, {"right", 106}, {"end", 107}, {"down", 108}, {"pagedown", 109}, {"pgdn", 109},
        {"insert", 110}, {"ins", 110},
        {"delete", 111}, {"del", 111}, {"forwarddelete", 111},
        {"kpequal", 117},
        {"leftmeta", 125}, {"lmeta", 125}, {"meta", 125}, {"super", 125}, {"leftsuper", 125}, {"lsuper", 125},
        {"cmd", 125}, {"command", 125}, {"leftcmd", 125}, {"leftcommand", 125}, {"lcmd", 125}, {"win", 125}, {"lwin", 125},
        {"rightmeta", 126}, {"rmeta", 126}, {"rightsuper", 126}, {"rsuper", 126},
        {"rcmd", 126}, {"rcommand", 126}, {"rightcmd", 126}, {"rightcommand", 126}, {"rwin", 126},
        {"globe", 464}, {"fn", 464}, {"function", 464},
    };
    for (const auto &entry : kNamed) {
        if (s == entry.name) {
            if (out_code) *out_code = entry.code;
            return true;
        }
    }
    return false;
}

struct KeyboardMappingConfig {
    KeyboardLayout layout = KeyboardLayout::kQwerty;
    std::map<uint32_t, uint32_t> keymap;

    bool operator==(const KeyboardMappingConfig &o) const {
        return layout == o.layout && keymap == o.keymap;
    }
    bool operator!=(const KeyboardMappingConfig &o) const { return !(*this == o); }
};

inline void ApplyKeyboardSettings(const std::string &kb_layout,
                                  const std::vector<std::pair<std::string, std::string>> &pairs,
                                  KeyboardMappingConfig *cfg) {
    if (!cfg) return;
    if (!kb_layout.empty()) {
        KeyboardLayout parsed = KeyboardLayout::kQwerty;
        if (ParseKeyboardLayoutName(kb_layout, &parsed)) cfg->layout = parsed;
    }
    for (const auto &kv : pairs) {
        uint32_t from = 0, to = 0;
        if (!ParseEvdevKeyName(kv.first, &from, false)) continue;
        if (!ParseEvdevKeyName(kv.second, &to, true)) continue;
        if (from == to) cfg->keymap.erase(from);
        else cfg->keymap[from] = to;
    }
}

// Translates Linux evdev keycodes (<linux/input-event-codes.h>) to Apple ScreensharingAgent
// X11 keysyms (verified against ScreensharingAgent KeyMapDictionary_ConvertKeysym):
//   - Super_L/R (0xffeb/0xffec) -> kVK_Command / kVK_RightCommand (0x37/0x36)
//   - Meta_L/R  (0xffe7/0xffe8) -> kVK_Option  / kVK_RightOption  (0x3a/0x3d)
//   - Printable ASCII keys carry their target character keysym ('!' vs '1', 'A' vs 'a',
//     and layout-translated characters for Dvorak / Colemak) because ScreensharingAgent builds
//     its reverse keysym-to-keycode dictionary from the Mac's active TIS keyboard layout.
//   - Applies optional physical-key remaps (`cfg_.keymap`, e.g. Caps Lock -> Left Control)
//     before Caps Lock toggle and modifier tracking, reference-counting shared target keysyms.
//   - Preserves Frametop's Meta holdback + KEY_F24 (194) combo suppression so Frametop hotkeys
//     (Meta+Shift+R, etc.) never leak Command taps to macOS.
class EvdevKeyMapper {
public:
    static constexpr uint32_t kEvKeyLeftShift = 42;
    static constexpr uint32_t kEvKeyRightShift = 54;
    static constexpr uint32_t kEvKeyCapsLock = 58;
    static constexpr uint32_t kEvKeyLeftMeta = 125;
    static constexpr uint32_t kEvKeyRightMeta = 126;
    static constexpr uint32_t kEvKeyF24 = 194;
    static constexpr uint32_t kEvKeyFn = 464;  // Linux KEY_FN (0x1d0) -> macOS Globe / Fn (kVK_Function = 0x3f)

    using EmitFn = std::function<void(bool down, uint32_t keysym)>;

    EvdevKeyMapper() = default;
    explicit EvdevKeyMapper(KeyboardMappingConfig cfg) : cfg_(std::move(cfg)) {}

    const KeyboardMappingConfig &config() const { return cfg_; }

    void SetConfig(const KeyboardMappingConfig &cfg, const EmitFn &emit) {
        if (cfg == cfg_) return;
        Blur(emit);
        if (cfg.keymap.find(kEvKeyCapsLock) != cfg.keymap.end()) caps_ = false;
        cfg_ = cfg;
    }

    static bool LookupLayoutAscii(KeyboardLayout layout, uint32_t evdev, char *unshifted, char *shifted) {
        if (evdev >= 2 && evdev <= 11) {
            static constexpr char kNum[11] = "1234567890";
            static constexpr char kNumS[11] = "!@#$%^&*()";
            *unshifted = kNum[evdev - 2];
            *shifted = kNumS[evdev - 2];
            return true;
        }
        if (evdev == 41) {
            *unshifted = '`';
            *shifted = '~';
            return true;
        }
        if (evdev == 43) {
            *unshifted = '\\';
            *shifted = '|';
            return true;
        }
        if (layout == KeyboardLayout::kDvorak) {
            if (evdev == 12) { *unshifted = '['; *shifted = '{'; return true; }
            if (evdev == 13) { *unshifted = ']'; *shifted = '}'; return true; }
            if (evdev >= 16 && evdev <= 25) {
                static constexpr char kRowQ[11] = "',.pyfgcrl";
                static constexpr char kRowQS[11] = "\"<>PYFGCRL";
                *unshifted = kRowQ[evdev - 16];
                *shifted = kRowQS[evdev - 16];
                return true;
            }
            if (evdev == 26) { *unshifted = '/'; *shifted = '?'; return true; }
            if (evdev == 27) { *unshifted = '='; *shifted = '+'; return true; }
            if (evdev >= 30 && evdev <= 39) {
                static constexpr char kRowA[11] = "aoeuidhtns";
                static constexpr char kRowAS[11] = "AOEUIDHTNS";
                *unshifted = kRowA[evdev - 30];
                *shifted = kRowAS[evdev - 30];
                return true;
            }
            if (evdev == 40) { *unshifted = '-'; *shifted = '_'; return true; }
            if (evdev == 44) { *unshifted = ';'; *shifted = ':'; return true; }
            if (evdev >= 45 && evdev <= 53) {
                static constexpr char kRowZ[10] = "qjkxbmwvz";
                static constexpr char kRowZS[10] = "QJKXBMWVZ";
                *unshifted = kRowZ[evdev - 45];
                *shifted = kRowZS[evdev - 45];
                return true;
            }
            return false;
        }
        if (layout == KeyboardLayout::kColemak) {
            if (evdev == 12) { *unshifted = '-'; *shifted = '_'; return true; }
            if (evdev == 13) { *unshifted = '='; *shifted = '+'; return true; }
            if (evdev >= 16 && evdev <= 24) {
                static constexpr char kRowQ[10] = "qwfpgjluy";
                static constexpr char kRowQS[10] = "QWFPGJLUY";
                *unshifted = kRowQ[evdev - 16];
                *shifted = kRowQS[evdev - 16];
                return true;
            }
            if (evdev == 25) { *unshifted = ';'; *shifted = ':'; return true; }
            if (evdev == 26) { *unshifted = '['; *shifted = '{'; return true; }
            if (evdev == 27) { *unshifted = ']'; *shifted = '}'; return true; }
            if (evdev >= 30 && evdev <= 39) {
                static constexpr char kRowA[11] = "arstdhneio";
                static constexpr char kRowAS[11] = "ARSTDHNEIO";
                *unshifted = kRowA[evdev - 30];
                *shifted = kRowAS[evdev - 30];
                return true;
            }
            if (evdev == 40) { *unshifted = '\''; *shifted = '"'; return true; }
            if (evdev >= 44 && evdev <= 50) {
                static constexpr char kRowZ[8] = "zxcvbkm";
                static constexpr char kRowZS[8] = "ZXCVBKM";
                *unshifted = kRowZ[evdev - 44];
                *shifted = kRowZS[evdev - 44];
                return true;
            }
            if (evdev == 51) { *unshifted = ','; *shifted = '<'; return true; }
            if (evdev == 52) { *unshifted = '.'; *shifted = '>'; return true; }
            if (evdev == 53) { *unshifted = '/'; *shifted = '?'; return true; }
            return false;
        }
        // Default ANSI QWERTY (also used for Command combos in kDvorakQwertyCmd).
        if (evdev == 12) { *unshifted = '-'; *shifted = '_'; return true; }
        if (evdev == 13) { *unshifted = '='; *shifted = '+'; return true; }
        if (evdev >= 16 && evdev <= 25) {
            static constexpr char kRowQ[11] = "qwertyuiop";
            static constexpr char kRowQS[11] = "QWERTYUIOP";
            *unshifted = kRowQ[evdev - 16];
            *shifted = kRowQS[evdev - 16];
            return true;
        }
        if (evdev == 26) { *unshifted = '['; *shifted = '{'; return true; }
        if (evdev == 27) { *unshifted = ']'; *shifted = '}'; return true; }
        if (evdev >= 30 && evdev <= 38) {
            static constexpr char kRowA[10] = "asdfghjkl";
            static constexpr char kRowAS[10] = "ASDFGHJKL";
            *unshifted = kRowA[evdev - 30];
            *shifted = kRowAS[evdev - 30];
            return true;
        }
        if (evdev == 39) { *unshifted = ';'; *shifted = ':'; return true; }
        if (evdev == 40) { *unshifted = '\''; *shifted = '"'; return true; }
        if (evdev >= 44 && evdev <= 50) {
            static constexpr char kRowZ[8] = "zxcvbnm";
            static constexpr char kRowZS[8] = "ZXCVBNM";
            *unshifted = kRowZ[evdev - 44];
            *shifted = kRowZS[evdev - 44];
            return true;
        }
        if (evdev == 51) { *unshifted = ','; *shifted = '<'; return true; }
        if (evdev == 52) { *unshifted = '.'; *shifted = '>'; return true; }
        if (evdev == 53) { *unshifted = '/'; *shifted = '?'; return true; }
        return false;
    }

    static uint32_t LookupKeysym(uint32_t evdev, bool shift, bool caps,
                                 KeyboardLayout layout = KeyboardLayout::kQwerty) {
        char unshifted = 0, shifted = 0;
        if (LookupLayoutAscii(layout, evdev, &unshifted, &shifted)) {
            const bool is_letter = (unshifted >= 'a' && unshifted <= 'z');
            const bool use_shifted = is_letter ? (shift != caps) : shift;
            return uint32_t(uint8_t(use_shifted ? shifted : unshifted));
        }
        switch (evdev) {
            case 1:   return 0xff1b;  // KEY_ESC -> Escape
            case 14:  return 0xff08;  // KEY_BACKSPACE -> BackSpace (kVK_Delete 0x33)
            case 15:  return 0xff09;  // KEY_TAB -> Tab
            case 28:  return 0xff0d;  // KEY_ENTER -> Return
            case 29:  return 0xffe3;  // KEY_LEFTCTRL -> Control_L (0x3b)
            case 42:  return 0xffe1;  // KEY_LEFTSHIFT -> Shift_L (0x38)
            case 54:  return 0xffe2;  // KEY_RIGHTSHIFT -> Shift_R (0x3c)
            case 55:  return 0xffaa;  // KEY_KPASTERISK -> KP_Multiply
            case 56:  return 0xffe7;  // KEY_LEFTALT -> Meta_L -> Mac Option_L (0x3a)
            case 57:  return 0x0020;  // KEY_SPACE
            case 58:  return 0xffe5;  // KEY_CAPSLOCK -> Caps_Lock (0x39)
            case 59: case 60: case 61: case 62: case 63:
            case 64: case 65: case 66: case 67: case 68:
                return 0xffbe + (evdev - 59);  // F1..F10
            case 69:  return 0xff7f;  // KEY_NUMLOCK -> Clear
            case 71:  return 0xffb7;  // KP_7
            case 72:  return 0xffb8;  // KP_8
            case 73:  return 0xffb9;  // KP_9
            case 74:  return 0xffad;  // KP_Subtract
            case 75:  return 0xffb4;  // KP_4
            case 76:  return 0xffb5;  // KP_5
            case 77:  return 0xffb6;  // KP_6
            case 78:  return 0xffab;  // KP_Add
            case 79:  return 0xffb1;  // KP_1
            case 80:  return 0xffb2;  // KP_2
            case 81:  return 0xffb3;  // KP_3
            case 82:  return 0xffb0;  // KP_0
            case 83:  return 0xffae;  // KP_Decimal
            case 86:  return shift ? '>' : '<';  // KEY_102ND
            case 87:  return 0xffc8;  // F11
            case 88:  return 0xffc9;  // F12
            case 96:  return 0xff8d;  // KP_Enter
            case 97:  return 0xffe4;  // KEY_RIGHTCTRL -> Control_R (0x3e)
            case 98:  return 0xffaf;  // KP_Divide
            case 100: return 0xffe8;  // KEY_RIGHTALT -> Meta_R -> Mac Option_R (0x3d)
            case 102: return 0xff50;  // KEY_HOME
            case 103: return 0xff52;  // KEY_UP
            case 104: return 0xff55;  // KEY_PAGEUP
            case 105: return 0xff51;  // KEY_LEFT
            case 106: return 0xff53;  // KEY_RIGHT
            case 107: return 0xff57;  // KEY_END
            case 108: return 0xff54;  // KEY_DOWN
            case 109: return 0xff56;  // KEY_PAGEDOWN
            case 110: return 0xff63;  // KEY_INSERT -> Insert/Help
            case 111: return 0xffff;  // KEY_DELETE -> Forward Delete (0x75)
            case 117: return 0xffbd;  // KEY_KPEQUAL -> KP_Equal
            case 125: return 0xffeb;  // KEY_LEFTMETA -> Super_L -> Mac Command_L (0x37)
            case 126: return 0xffec;  // KEY_RIGHTMETA -> Super_R -> Mac Command_R (0x36)
            case 183: case 184: case 185: case 186:
            case 187: case 188: case 189: case 190:
                return 0xffca + (evdev - 183);  // F13..F20
            case 464: return kAppleKeysymGlobe; // KEY_FN / "globe" -> kVK_Function (0x3f) via 0x10 packet
            default:  return 0;
        }
    }

    void Process(uint32_t raw_code, bool down, const EmitFn &emit) {
        if (raw_code == kEvKeyF24) {
            held_meta_raw_ = 0;  // Frametop ate a Meta combo; suppress held Command.
            held_meta_code_ = 0;
            return;
        }

        if (!down) {
            if (suppressed_raw_.erase(raw_code) > 0) return;
            if (held_meta_raw_ != 0 && raw_code == held_meta_raw_) {
                const uint32_t mcode = held_meta_code_;
                held_meta_raw_ = 0;
                held_meta_code_ = 0;
                const uint32_t mks = LookupKeysym(mcode, false, false, EffectiveLayout());
                if (mks) {
                    emit(true, mks);
                    emit(false, mks);
                }
                return;
            }
            const auto it = active_raw_.find(raw_code);
            if (it == active_raw_.end()) return;
            const ActiveKey entry = it->second;
            active_raw_.erase(it);
            if ((entry.mapped_code == kEvKeyLeftShift || entry.mapped_code == kEvKeyRightShift) && shift_refs_ > 0) {
                --shift_refs_;
            }
            if ((entry.mapped_code == kEvKeyLeftMeta || entry.mapped_code == kEvKeyRightMeta) && meta_refs_ > 0) {
                --meta_refs_;
            }
            const auto kit = keysym_refs_.find(entry.keysym);
            if (kit != keysym_refs_.end()) {
                if (--kit->second == 0) {
                    keysym_refs_.erase(kit);
                    emit(false, entry.keysym);
                }
            }
            return;
        }

        // Key-down: handle auto-repeat on an already-active or held-back physical key cleanly.
        if (held_meta_raw_ != 0 && raw_code == held_meta_raw_) return;
        const auto existing = active_raw_.find(raw_code);
        if (existing != active_raw_.end()) {
            emit(true, existing->second.keysym);
            return;
        }

        uint32_t code = raw_code;
        const auto map_it = cfg_.keymap.find(raw_code);
        if (map_it != cfg_.keymap.end()) {
            if (map_it->second == 0) {
                suppressed_raw_.insert(raw_code);
                return;
            }
            code = map_it->second;
        }

        const bool is_shift = (code == kEvKeyLeftShift || code == kEvKeyRightShift);
        const bool is_meta = (code == kEvKeyLeftMeta || code == kEvKeyRightMeta);
        if (code == kEvKeyCapsLock) caps_ = !caps_;

        if (is_meta && meta_refs_ == 0 && held_meta_raw_ == 0) {
            held_meta_raw_ = raw_code;
            held_meta_code_ = code;
            return;
        }
        if (held_meta_raw_ != 0) {
            const uint32_t mraw = held_meta_raw_;
            const uint32_t mcode = held_meta_code_;
            held_meta_raw_ = 0;
            held_meta_code_ = 0;
            const uint32_t mks = LookupKeysym(mcode, false, false, KeyboardLayout::kQwerty);
            if (mks) {
                active_raw_[mraw] = ActiveKey{mcode, mks};
                ++meta_refs_;
                if (++keysym_refs_[mks] == 1) emit(true, mks);
            }
        }

        if (is_shift) ++shift_refs_;
        if (is_meta) ++meta_refs_;
        const uint32_t ks = LookupKeysym(code, shift_refs_ > 0, caps_, EffectiveLayout());
        if (!ks) {
            if (is_shift && shift_refs_ > 0) --shift_refs_;
            if (is_meta && meta_refs_ > 0) --meta_refs_;
            return;
        }
        active_raw_[raw_code] = ActiveKey{code, ks};
        if (++keysym_refs_[ks] == 1) emit(true, ks);
    }

    void Blur(const EmitFn &emit) {
        held_meta_raw_ = 0;
        held_meta_code_ = 0;
        shift_refs_ = 0;
        meta_refs_ = 0;
        active_raw_.clear();
        suppressed_raw_.clear();
        for (const auto &kv : keysym_refs_) emit(false, kv.first);
        keysym_refs_.clear();
    }

private:
    struct ActiveKey {
        uint32_t mapped_code = 0;
        uint32_t keysym = 0;
    };

    KeyboardLayout EffectiveLayout() const {
        if (cfg_.layout == KeyboardLayout::kDvorakQwertyCmd) {
            return (meta_refs_ > 0 || held_meta_raw_ != 0) ? KeyboardLayout::kQwerty : KeyboardLayout::kDvorak;
        }
        return cfg_.layout;
    }

    KeyboardMappingConfig cfg_{};
    uint32_t held_meta_raw_ = 0;
    uint32_t held_meta_code_ = 0;
    uint32_t shift_refs_ = 0;
    uint32_t meta_refs_ = 0;
    bool caps_ = false;
    std::map<uint32_t, ActiveKey> active_raw_;
    std::map<uint32_t, uint32_t> keysym_refs_;
    std::set<uint32_t> suppressed_raw_;
};

// ── RFC 3640 AAC-ELD RTP Depacketizer & Private Credential Store ─────────────

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

inline std::string AppleCredsDir() {
    const char *home = std::getenv("HOME");
    return std::string(home && *home ? home : "/tmp") + "/.local/share/frametop-stream/hosts";
}

inline std::string SanitizeCredHost(const std::string &host) {
    std::string out;
    out.reserve(host.size());
    for (char c : host) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_') {
            out.push_back(c);
        }
    }
    return out.empty() ? "host" : out;
}

inline std::string AppleCredsPath(const std::string &host, const std::string &base_dir = "") {
    const std::string dir = base_dir.empty() ? AppleCredsDir() : base_dir;
    return dir + "/" + SanitizeCredHost(host) + ".apple";
}

inline bool EnsurePrivateDir(const std::string &dir) {
    for (size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos == dir.size() || dir[pos] == '/') {
            const std::string sub = dir.substr(0, pos);
            if (!sub.empty() && mkdir(sub.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    chmod(dir.c_str(), 0700);
    return true;
}

inline bool SaveAppleCreds(const std::string &path, const std::string &username, const std::string &password) {
    if (username.empty() || password.empty()) return false;
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && !EnsurePrivateDir(path.substr(0, slash))) return false;
    const std::string tmp = path + ".tmp." + std::to_string(getpid());
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = open(tmp.c_str(), flags, 0600);
    if (fd < 0) return false;
    fchmod(fd, 0600);
    std::string body = username + "\n" + password + "\n";
    const ssize_t n = write(fd, body.data(), body.size());
    OPENSSL_cleanse(body.data(), body.size());
    const int sync_rc = fsync(fd);
    close(fd);
    if (n != ssize_t(username.size() + password.size() + 2) || sync_rc != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

inline bool LoadAppleCreds(const std::string &path, std::string *username_out, std::string *password_out) {
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = open(path.c_str(), flags);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0077) != 0 || st.st_size <= 0 || st.st_size > 4096) {
        close(fd);
        return false;
    }
    std::string buf(size_t(st.st_size), '\0');
    const ssize_t n = read(fd, buf.data(), buf.size());
    close(fd);
    if (n <= 0) {
        OPENSSL_cleanse(buf.data(), buf.size());
        return false;
    }
    buf.resize(size_t(n));
    const size_t nl1 = buf.find('\n');
    if (nl1 == std::string::npos) {
        OPENSSL_cleanse(buf.data(), buf.size());
        return false;
    }
    std::string u = buf.substr(0, nl1);
    if (!u.empty() && u.back() == '\r') u.pop_back();
    const size_t nl2 = buf.find('\n', nl1 + 1);
    std::string p = (nl2 == std::string::npos) ? buf.substr(nl1 + 1) : buf.substr(nl1 + 1, nl2 - (nl1 + 1));
    if (!p.empty() && p.back() == '\r') p.pop_back();
    OPENSSL_cleanse(buf.data(), buf.size());
    if (u.empty() || p.empty()) {
        OPENSSL_cleanse(p.data(), p.size());
        return false;
    }
    *username_out = std::move(u);
    *password_out = std::move(p);
    return true;
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

// ── SRP-6a (RFC 5054 4096-bit + SHA-512 + PBKDF2) & Non-SRP RSA1+AES ─────────

struct SrpChallenge {
    std::array<uint8_t, kSrpModulusLen> N{};
    std::vector<uint8_t> g;
    std::vector<uint8_t> salt;
    std::vector<uint8_t> B;
    uint64_t iterations = 0;
    std::vector<uint8_t> cap;
};

struct SrpClientProof {
    std::array<uint8_t, kSrpModulusLen> A{};
    std::array<uint8_t, kSrpHashLen> M1{};
    std::array<uint8_t, kSrpHashLen> K{};
    std::array<uint8_t, 16> wrap_key{};
};

inline bool ParseSrpChallenge(const uint8_t *s2c1, size_t len, SrpChallenge *out) {
    if (len < 1000 || s2c1[12] != 0x00) return false;
    size_t p = 13;
    if (p + kSrpModulusLen + 2 > len) return false;
    std::memcpy(out->N.data(), s2c1 + p, kSrpModulusLen);
    p += kSrpModulusLen;

    const uint16_t g_len = ReadBe16(s2c1 + p);
    p += 2;
    if (g_len == 0 || g_len > kSrpModulusLen || p + g_len + 1 > len) return false;
    out->g.assign(s2c1 + p, s2c1 + p + g_len);
    p += g_len;

    const uint8_t salt_len = s2c1[p++];
    if (salt_len == 0 || p + salt_len + 2 > len) return false;
    out->salt.assign(s2c1 + p, s2c1 + p + salt_len);
    p += salt_len;

    const uint16_t b_len = ReadBe16(s2c1 + p);
    p += 2;
    if (b_len == 0 || b_len > kSrpModulusLen || p + b_len + 10 > len) return false;
    out->B.assign(s2c1 + p, s2c1 + p + b_len);
    p += b_len;

    out->iterations = ReadBe64(s2c1 + p);
    p += 8;
    if (out->iterations == 0 || out->iterations > 1'000'000) return false;

    const uint16_t cap_len = ReadBe16(s2c1 + p);
    p += 2;
    if (p + cap_len > len) return false;
    out->cap.assign(s2c1 + p, s2c1 + p + cap_len);
    return true;
}

inline UniqueBn DeriveSrpX(const std::vector<uint8_t> &salt, uint64_t iterations, const std::string &password) {
    uint8_t dk[128];
    if (!Pbkdf2HmacSha512(password, salt, iterations, dk)) return nullptr;
    const uint8_t colon = ':';
    const auto inner = Sha512Parts({{&colon, 1}, {dk, sizeof dk}});
    OPENSSL_cleanse(dk, sizeof dk);
    const auto x_hash = Sha512Parts({{salt.data(), salt.size()}, {inner.data(), inner.size()}});
    return UniqueBn(BN_bin2bn(x_hash.data(), int(x_hash.size()), nullptr));
}

inline bool SolveSrp(const SrpChallenge &ch, const std::string &password, SrpClientProof *out,
                     const uint8_t *fixed_a64 = nullptr) {
    UniqueBnCtx ctx(BN_CTX_new());
    UniqueBn bn_N(BN_bin2bn(ch.N.data(), int(ch.N.size()), nullptr));
    UniqueBn bn_g(BN_bin2bn(ch.g.data(), int(ch.g.size()), nullptr));
    UniqueBn bn_B(BN_bin2bn(ch.B.data(), int(ch.B.size()), nullptr));
    if (!ctx || !bn_N || !bn_g || !bn_B || BN_is_zero(bn_N.get())) return false;

    UniqueBn b_mod_n(BN_new());
    if (!b_mod_n || BN_mod(b_mod_n.get(), bn_B.get(), bn_N.get(), ctx.get()) != 1 || BN_is_zero(b_mod_n.get()))
        return false;

    std::array<uint8_t, kSrpModulusLen> g_pad{}, B_pad{};
    if (BN_bn2binpad(bn_g.get(), g_pad.data(), int(kSrpModulusLen)) < 0) return false;
    if (BN_bn2binpad(bn_B.get(), B_pad.data(), int(kSrpModulusLen)) < 0) return false;

    const auto k_hash = Sha512Parts({{ch.N.data(), ch.N.size()}, {g_pad.data(), g_pad.size()}});
    UniqueBn bn_k(BN_bin2bn(k_hash.data(), int(k_hash.size()), nullptr));

    uint8_t a_raw[64];
    if (fixed_a64) std::memcpy(a_raw, fixed_a64, 64);
    else if (!RandomBytes(a_raw, sizeof a_raw)) return false;

    UniqueBn bn_nm1(BN_dup(bn_N.get()));
    UniqueBn bn_a_in(BN_bin2bn(a_raw, sizeof a_raw, nullptr));
    UniqueBn bn_a(BN_new());
    UniqueBn bn_A(BN_new());
    if (!bn_nm1 || !bn_a_in || !bn_a || !bn_A) return false;
    BN_sub_word(bn_nm1.get(), 1);
    if (BN_mod(bn_a.get(), bn_a_in.get(), bn_nm1.get(), ctx.get()) != 1) return false;
    BN_add_word(bn_a.get(), 1);
    if (BN_mod_exp(bn_A.get(), bn_g.get(), bn_a.get(), bn_N.get(), ctx.get()) != 1) return false;
    if (BN_bn2binpad(bn_A.get(), out->A.data(), int(kSrpModulusLen)) < 0) return false;

    const auto u_hash = Sha512Parts({{out->A.data(), out->A.size()}, {B_pad.data(), B_pad.size()}});
    UniqueBn bn_u(BN_bin2bn(u_hash.data(), int(u_hash.size()), nullptr));
    UniqueBn bn_x_raw = DeriveSrpX(ch.salt, ch.iterations, password);
    UniqueBn bn_x(BN_new());
    if (!bn_u || !bn_x_raw || !bn_x) return false;
    if (BN_mod(bn_x.get(), bn_x_raw.get(), bn_N.get(), ctx.get()) != 1) return false;

    // S = ((B - k * g^x mod N) % N) ^ (a + u * x) mod N
    UniqueBn bn_gx(BN_new()), bn_kgx(BN_new()), bn_base(BN_new()), bn_ux(BN_new()), bn_exp(BN_new()), bn_S(BN_new());
    if (!bn_gx || !bn_kgx || !bn_base || !bn_ux || !bn_exp || !bn_S) return false;
    if (BN_mod_exp(bn_gx.get(), bn_g.get(), bn_x.get(), bn_N.get(), ctx.get()) != 1) return false;
    if (BN_mod_mul(bn_kgx.get(), bn_k.get(), bn_gx.get(), bn_N.get(), ctx.get()) != 1) return false;
    if (BN_mod_sub(bn_base.get(), b_mod_n.get(), bn_kgx.get(), bn_N.get(), ctx.get()) != 1) return false;
    if (BN_mul(bn_ux.get(), bn_u.get(), bn_x.get(), ctx.get()) != 1) return false;
    if (BN_add(bn_exp.get(), bn_a.get(), bn_ux.get()) != 1) return false;
    if (BN_mod_exp(bn_S.get(), bn_base.get(), bn_exp.get(), bn_N.get(), ctx.get()) != 1) return false;

    std::array<uint8_t, kSrpModulusLen> S_pad{};
    if (BN_bn2binpad(bn_S.get(), S_pad.data(), int(kSrpModulusLen)) < 0) return false;
    out->K = Sha512Parts({{S_pad.data(), S_pad.size()}});
    OPENSSL_cleanse(S_pad.data(), S_pad.size());

    const auto h_N = Sha512Parts({{ch.N.data(), ch.N.size()}});
    const auto h_g = Sha512Parts({{g_pad.data(), g_pad.size()}});
    std::array<uint8_t, kSrpHashLen> hn_xor_hg{};
    for (size_t i = 0; i < kSrpHashLen; ++i) hn_xor_hg[i] = h_N[i] ^ h_g[i];
    const auto h_empty = Sha512Parts({});

    out->M1 = Sha512Parts({
        {hn_xor_hg.data(), hn_xor_hg.size()},
        {h_empty.data(), h_empty.size()},
        {ch.salt.data(), ch.salt.size()},
        {out->A.data(), out->A.size()},
        {B_pad.data(), B_pad.size()},
        {out->K.data(), out->K.size()},
    });
    const auto sha256_k = Sha256(out->K.data(), out->K.size());
    std::memcpy(out->wrap_key.data(), sha256_k.data(), 16);
    return true;
}

inline bool BuildSrpPacket1(EVP_PKEY *server_pub, const std::string &username, std::vector<uint8_t> *pkt_out) {
    std::vector<uint8_t> inner;
    AppendBe32(inner, uint32_t(username.size()));
    inner.insert(inner.end(), username.begin(), username.end());
    inner.insert(inner.end(), {0x00, 0x00, 0x00});

    std::vector<uint8_t> plain;
    AppendBe32(plain, uint32_t(inner.size()));
    plain.insert(plain.end(), inner.begin(), inner.end());

    std::vector<uint8_t> enc;
    if (!RsaPkcs1Encrypt(server_pub, plain.data(), plain.size(), &enc) || enc.size() != 256) return false;

    pkt_out->clear();
    AppendBe32(*pkt_out, 650);
    pkt_out->insert(pkt_out->end(), {0x01, 0x00, 'R', 'S', 'A', '1', 0x00, 0x02});
    AppendBe16(*pkt_out, 256);
    pkt_out->insert(pkt_out->end(), enc.begin(), enc.end());
    pkt_out->insert(pkt_out->end(), 384, 0x00);
    return true;
}

inline std::vector<uint8_t> BuildSrpPacket2(const SrpChallenge &ch, const SrpClientProof &proof) {
    uint8_t civ[16];
    RandomBytes(civ, sizeof civ);

    std::vector<uint8_t> sd;
    AppendBe16(sd, uint16_t(kSrpModulusLen));
    sd.insert(sd.end(), proof.A.begin(), proof.A.end());
    sd.push_back(uint8_t(kSrpHashLen));
    sd.insert(sd.end(), proof.M1.begin(), proof.M1.end());
    AppendBe16(sd, uint16_t(ch.cap.size()));
    sd.insert(sd.end(), ch.cap.begin(), ch.cap.end());
    sd.push_back(16);
    sd.insert(sd.end(), civ, civ + 16);

    std::vector<uint8_t> pay = {0x01, 0x00, 'R', 'S', 'A', '1', 0x00, 0x02};
    AppendBe16(pay, uint16_t(sd.size() + 4));
    AppendBe32(pay, uint32_t(sd.size()));
    pay.insert(pay.end(), sd.begin(), sd.end());
    if (pay.size() < 1076) pay.resize(1076, 0x00);

    std::vector<uint8_t> out;
    AppendBe32(out, uint32_t(pay.size()));
    out.insert(out.end(), pay.begin(), pay.end());
    return out;
}

inline bool BuildNonSrpPacket(EVP_PKEY *server_pub, const std::string &username, const std::string &password,
                              const uint8_t aes_key16[16], std::vector<uint8_t> *pkt_out) {
    auto pack64 = [](const std::string &s) {
        std::array<uint8_t, 64> slot{};
        RandomBytes(slot.data(), slot.size());
        const size_t n = std::min<size_t>(63, s.size());
        std::memcpy(slot.data(), s.data(), n);
        slot[n] = 0;
        return slot;
    };
    const auto u64 = pack64(username);
    const auto p64 = pack64(password);
    uint8_t creds[128], enc_creds[128];
    std::memcpy(creds, u64.data(), 64);
    std::memcpy(creds + 64, p64.data(), 64);
    const bool ecb_ok = Aes128EcbCrypt(true, aes_key16, creds, 128, enc_creds);
    OPENSSL_cleanse(creds, sizeof creds);
    if (!ecb_ok) return false;

    std::vector<uint8_t> enc_key;
    if (!RsaPkcs1Encrypt(server_pub, aes_key16, 16, &enc_key) || enc_key.size() != 256) return false;

    std::vector<uint8_t> blob = {0x01, 0x00, 'R', 'S', 'A', '1', 0x00, 0x01};
    blob.insert(blob.end(), enc_creds, enc_creds + 128);
    blob.push_back(0x00);
    blob.push_back(0x01);
    blob.insert(blob.end(), enc_key.begin(), enc_key.end());

    pkt_out->clear();
    AppendBe32(*pkt_out, uint32_t(blob.size()));
    pkt_out->insert(pkt_out->end(), blob.begin(), blob.end());
    return true;
}

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

        auto &st = ssrc_states_[ssrc];
        uint32_t est_roc = st.roc;
        int32_t delta_roc = 0;
        if (!st.initialized) {
            est_roc = 0;
        } else if (st.max_seq < 32768) {
            if (int32_t(seq) - int32_t(st.max_seq) > 32768 && st.roc > 0) {
                est_roc = st.roc - 1;
                delta_roc = -1;
            }
        } else {
            if (int32_t(st.max_seq) - 32768 > int32_t(seq)) {
                est_roc = st.roc + 1;
                delta_roc = 1;
            }
        }

        uint8_t roc_be[4], mac20[20];
        WriteBe32(roc_be, est_roc);
        if (!HmacSha1(rtp_auth_.data(), rtp_auth_.size(), pkt, auth_portion_len, roc_be, 4, mac20)) return false;
        if (CRYPTO_memcmp(mac20, pkt + auth_portion_len, kSrtpTagLen) != 0) return false;

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

// ── H.264 Bitstream Surgery, 0x92 avcC Extractor, & RTP Depacketizer ─────────

inline std::vector<uint8_t> RemoveEmulationPrevention(const uint8_t *ebsp, size_t len) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(len);
    size_t zeros = 0;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = ebsp[i];
        if (zeros == 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(b);
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }
    return rbsp;
}

inline std::vector<uint8_t> AddEmulationPrevention(const std::vector<uint8_t> &rbsp) {
    std::vector<uint8_t> ebsp;
    ebsp.reserve(rbsp.size() + 8);
    size_t zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros == 2 && b <= 0x03) {
            ebsp.push_back(0x03);
            zeros = 0;
        }
        ebsp.push_back(b);
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }
    return ebsp;
}

class BitReader {
public:
    BitReader(const uint8_t *data, size_t len) : data_(data), total_bits_(len * 8) {}

    size_t pos() const { return pos_; }
    size_t total_bits() const { return total_bits_; }

    bool ReadBits(size_t n, uint32_t *out) {
        if (n > 32 || pos_ + n > total_bits_) return false;
        uint32_t v = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t bit = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u;
            v = (v << 1) | bit;
            ++pos_;
        }
        *out = v;
        return true;
    }

    bool ReadUe(uint32_t *out, size_t *start_bit = nullptr, size_t *end_bit = nullptr) {
        const size_t s = pos_;
        size_t z = 0;
        while (pos_ < total_bits_) {
            const uint8_t bit = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u;
            ++pos_;
            if (bit == 1) break;
            ++z;
            if (z > 31) return false;
        }
        if (z > 31 || (pos_ == total_bits_ && z > 0 && ((data_[(pos_ - 1) >> 3] >> (7 - ((pos_ - 1) & 7))) & 1u) == 0))
            return false;
        uint32_t suffix = 0;
        if (z > 0 && !ReadBits(z, &suffix)) return false;
        *out = ((1u << z) - 1u) + suffix;
        if (start_bit) *start_bit = s;
        if (end_bit) *end_bit = pos_;
        return true;
    }

    bool ReadSe(int32_t *out) {
        uint32_t code = 0;
        if (!ReadUe(&code)) return false;
        const int32_t mag = int32_t((code + 1) >> 1);
        *out = (code & 1u) ? mag : -mag;
        return true;
    }

private:
    const uint8_t *data_;
    size_t total_bits_;
    size_t pos_ = 0;
};

class BitWriter {
public:
    void WriteBit(uint8_t b) {
        if ((bits_ & 7) == 0) buf_.push_back(0);
        if (b & 1u) buf_.back() |= uint8_t(1u << (7 - (bits_ & 7)));
        ++bits_;
    }

    void WriteBits(uint32_t v, size_t n) {
        for (size_t i = 0; i < n; ++i) WriteBit(uint8_t((v >> (n - 1 - i)) & 1u));
    }

    void WriteUe(uint32_t v) {
        const uint32_t code = v + 1u;
        size_t bits = 0;
        for (uint32_t t = code; t > 0; t >>= 1) ++bits;
        const size_t z = bits - 1;
        for (size_t i = 0; i < z; ++i) WriteBit(0);
        WriteBits(code, bits);
    }

    void CopyBitRange(const uint8_t *src, size_t begin_bit, size_t end_bit) {
        for (size_t p = begin_bit; p < end_bit; ++p) {
            WriteBit((src[p >> 3] >> (7 - (p & 7))) & 1u);
        }
    }

    // Re-aligns RBSP trailing stop bit (`1` followed by `0`s) to a byte boundary after a bit-length change.
    bool CopyRemainingRbspWithRealignedStopBit(const uint8_t *src_rbsp, size_t src_bytes, size_t from_bit) {
        if (src_bytes == 0) return false;
        size_t stop_bit = src_bytes * 8;
        while (stop_bit > from_bit) {
            --stop_bit;
            if (((src_rbsp[stop_bit >> 3] >> (7 - (stop_bit & 7))) & 1u) == 1u) break;
        }
        if (((src_rbsp[stop_bit >> 3] >> (7 - (stop_bit & 7))) & 1u) != 1u || stop_bit < from_bit) return false;
        CopyBitRange(src_rbsp, from_bit, stop_bit);
        WriteBit(1);
        while ((bits_ & 7) != 0) WriteBit(0);
        return true;
    }

    const std::vector<uint8_t> &bytes() const { return buf_; }

private:
    std::vector<uint8_t> buf_;
    size_t bits_ = 0;
};

struct H264SpsInfo {
    uint8_t profile_idc = 0;
    uint8_t level_idc = 0;
    uint32_t max_num_ref_frames = 0;
    uint32_t max_num_reorder_frames = 0xffffffffu;
    uint32_t max_dec_frame_buffering = 0;
    bool has_bitstream_restriction = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t log2_max_frame_num_minus4 = 0;
    uint32_t pic_order_cnt_type = 0;
    uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;
    bool frame_mbs_only_flag = true;
    bool separate_colour_plane_flag = false;
    size_t poc_start_bit = 0;
    size_t poc_end_bit = 0;
    size_t ref_start_bit = 0;
    size_t ref_end_bit = 0;
    size_t vui_flag_bit = 0;
    bool vui_present = false;
    size_t br_flag_bit = 0;  // Valid when vui_present == true
};

struct H264PpsInfo {
    bool valid = false;
    bool entropy_coding_mode_flag = false;
    bool bottom_field_pic_order_in_frame_present_flag = false;
    bool weighted_pred_flag = false;
    bool deblocking_filter_control_present_flag = false;
    bool redundant_pic_cnt_present_flag = false;
};

inline bool SkipHrdParameters(BitReader &br) {
    uint32_t cpb_cnt_m1 = 0, tmp = 0;
    if (!br.ReadUe(&cpb_cnt_m1) || cpb_cnt_m1 > 31) return false;
    if (!br.ReadBits(8, &tmp)) return false;  // bit_rate_scale(4) + cpb_size_scale(4)
    for (uint32_t i = 0; i <= cpb_cnt_m1; ++i) {
        uint32_t br_val = 0, cpb_val = 0, cbr = 0;
        if (!br.ReadUe(&br_val) || !br.ReadUe(&cpb_val) || !br.ReadBits(1, &cbr)) return false;
    }
    return br.ReadBits(20, &tmp);  // 4 x u(5)
}

inline bool SkipScalingList(BitReader &br, size_t size) {
    int32_t last_scale = 8, next_scale = 8;
    for (size_t j = 0; j < size; ++j) {
        if (next_scale != 0) {
            int32_t delta = 0;
            if (!br.ReadSe(&delta)) return false;
            next_scale = (last_scale + delta + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
    return true;
}

inline bool InspectH264SpsRbsp(const std::vector<uint8_t> &rbsp, H264SpsInfo *info) {
    if (rbsp.size() < 3) return false;
    BitReader br(rbsp.data(), rbsp.size());
    uint32_t profile = 0, flags = 0, level = 0, sps_id = 0;
    if (!br.ReadBits(8, &profile) || !br.ReadBits(8, &flags) || !br.ReadBits(8, &level) || !br.ReadUe(&sps_id))
        return false;

    uint32_t chroma_format_idc = 1;
    uint32_t sep_colour_plane = 0;
    static constexpr uint32_t kHighProfiles[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    if (std::find(std::begin(kHighProfiles), std::end(kHighProfiles), profile) != std::end(kHighProfiles)) {
        if (!br.ReadUe(&chroma_format_idc)) return false;
        if (chroma_format_idc == 3) {
            if (!br.ReadBits(1, &sep_colour_plane)) return false;
        }
        uint32_t luma_bd = 0, chroma_bd = 0, qpprime = 0, seq_scaling_present = 0;
        if (!br.ReadUe(&luma_bd) || !br.ReadUe(&chroma_bd) || !br.ReadBits(1, &qpprime) ||
            !br.ReadBits(1, &seq_scaling_present)) {
            return false;
        }
        if (seq_scaling_present) {
            const size_t cnt = (chroma_format_idc != 3) ? 8 : 12;
            for (size_t i = 0; i < cnt; ++i) {
                uint32_t list_present = 0;
                if (!br.ReadBits(1, &list_present)) return false;
                if (list_present && !SkipScalingList(br, i < 6 ? 16 : 64)) return false;
            }
        }
    }

    uint32_t log2_max_fn = 0, poc_type = 0, log2_max_poc = 0;
    if (!br.ReadUe(&log2_max_fn)) return false;
    const size_t poc_s = br.pos();
    if (!br.ReadUe(&poc_type)) return false;
    if (poc_type == 0) {
        if (!br.ReadUe(&log2_max_poc)) return false;
    } else if (poc_type == 1) {
        uint32_t delta_always_zero = 0, num_cycle = 0;
        int32_t off_non_ref = 0, off_top_bot = 0;
        if (!br.ReadBits(1, &delta_always_zero) || !br.ReadSe(&off_non_ref) ||
            !br.ReadSe(&off_top_bot) || !br.ReadUe(&num_cycle)) {
            return false;
        }
        for (uint32_t i = 0; i < num_cycle; ++i) {
            int32_t off = 0;
            if (!br.ReadSe(&off)) return false;
        }
    } else if (poc_type != 2) {
        return false;
    }
    const size_t poc_e = br.pos();

    uint32_t max_refs = 0;
    size_t ref_s = 0, ref_e = 0;
    if (!br.ReadUe(&max_refs, &ref_s, &ref_e)) return false;

    uint32_t gaps = 0, pic_w_mbs_m1 = 0, pic_h_map_m1 = 0, frame_mbs_only = 0;
    if (!br.ReadBits(1, &gaps) || !br.ReadUe(&pic_w_mbs_m1) || !br.ReadUe(&pic_h_map_m1) ||
        !br.ReadBits(1, &frame_mbs_only)) {
        return false;
    }
    if (!frame_mbs_only) {
        uint32_t mb_aff = 0;
        if (!br.ReadBits(1, &mb_aff)) return false;
    }
    uint32_t direct_8x8 = 0, crop = 0;
    if (!br.ReadBits(1, &direct_8x8) || !br.ReadBits(1, &crop)) return false;
    uint32_t crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (crop) {
        if (!br.ReadUe(&crop_l) || !br.ReadUe(&crop_r) || !br.ReadUe(&crop_t) || !br.ReadUe(&crop_b)) return false;
    }

    uint32_t w = (pic_w_mbs_m1 + 1) * 16;
    uint32_t h = (pic_h_map_m1 + 1) * 16 * (2 - frame_mbs_only);
    const uint32_t sub_w = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
    const uint32_t sub_h = (chroma_format_idc == 1 ? 2 : 1) * (2 - frame_mbs_only);
    if (crop) {
        const uint32_t dx = sub_w * (crop_l + crop_r);
        const uint32_t dy = sub_h * (crop_t + crop_b);
        if (dx < w && dy < h) {
            w -= dx;
            h -= dy;
        }
    }

    const size_t vui_flag_pos = br.pos();
    uint32_t vui_present = 0;
    if (!br.ReadBits(1, &vui_present)) return false;

    size_t br_flag_pos = 0;
    bool has_br = false;
    uint32_t max_reorder = 0xffffffffu;
    uint32_t max_dpb_buf = 0;

    if (vui_present) {
        uint32_t f = 0, v = 0;
        if (!br.ReadBits(1, &f)) return false;  // aspect_ratio_info_present_flag
        if (f) {
            if (!br.ReadBits(8, &v)) return false;
            if (v == 255 && !br.ReadBits(32, &v)) return false;
        }
        if (!br.ReadBits(1, &f)) return false;  // overscan_info_present_flag
        if (f && !br.ReadBits(1, &v)) return false;
        if (!br.ReadBits(1, &f)) return false;  // video_signal_type_present_flag
        if (f) {
            if (!br.ReadBits(4, &v) || !br.ReadBits(1, &f)) return false;
            if (f && !br.ReadBits(24, &v)) return false;
        }
        if (!br.ReadBits(1, &f)) return false;  // chroma_loc_info_present_flag
        if (f && (!br.ReadUe(&v) || !br.ReadUe(&v))) return false;
        if (!br.ReadBits(1, &f)) return false;  // timing_info_present_flag
        if (f && (!br.ReadBits(32, &v) || !br.ReadBits(32, &v) || !br.ReadBits(1, &v))) return false;
        uint32_t nal_hrd = 0, vcl_hrd = 0;
        if (!br.ReadBits(1, &nal_hrd)) return false;
        if (nal_hrd && !SkipHrdParameters(br)) return false;
        if (!br.ReadBits(1, &vcl_hrd)) return false;
        if (vcl_hrd && !SkipHrdParameters(br)) return false;
        if ((nal_hrd || vcl_hrd) && !br.ReadBits(1, &v)) return false;
        if (!br.ReadBits(1, &v)) return false;  // pic_struct_present_flag

        br_flag_pos = br.pos();
        uint32_t br_flag = 0;
        if (!br.ReadBits(1, &br_flag)) return false;
        if (br_flag) {
            has_br = true;
            if (!br.ReadBits(1, &v) || !br.ReadUe(&v) || !br.ReadUe(&v) ||
                !br.ReadUe(&v) || !br.ReadUe(&v) ||
                !br.ReadUe(&max_reorder) || !br.ReadUe(&max_dpb_buf)) {
                return false;
            }
        }
    }

    info->profile_idc = uint8_t(profile);
    info->level_idc = uint8_t(level);
    info->max_num_ref_frames = max_refs;
    info->max_num_reorder_frames = max_reorder;
    info->max_dec_frame_buffering = max_dpb_buf;
    info->has_bitstream_restriction = has_br;
    info->width = w;
    info->height = h;
    info->log2_max_frame_num_minus4 = log2_max_fn;
    info->pic_order_cnt_type = poc_type;
    info->log2_max_pic_order_cnt_lsb_minus4 = log2_max_poc;
    info->frame_mbs_only_flag = (frame_mbs_only != 0);
    info->separate_colour_plane_flag = (sep_colour_plane != 0);
    info->poc_start_bit = poc_s;
    info->poc_end_bit = poc_e;
    info->ref_start_bit = ref_s;
    info->ref_end_bit = ref_e;
    info->vui_flag_bit = vui_flag_pos;
    info->vui_present = (vui_present != 0);
    info->br_flag_bit = br_flag_pos;
    return true;
}

inline bool InspectH264Sps(const std::vector<uint8_t> &sps_nal, H264SpsInfo *info) {
    if (sps_nal.size() < 4 || (sps_nal[0] & 0x1f) != 7) return false;
    const auto rbsp = RemoveEmulationPrevention(sps_nal.data() + 1, sps_nal.size() - 1);
    return InspectH264SpsRbsp(rbsp, info);
}

inline bool InspectH264Pps(const std::vector<uint8_t> &pps_nal, H264PpsInfo *out) {
    *out = H264PpsInfo{};
    if (pps_nal.size() < 2 || (pps_nal[0] & 0x1f) != 8) return false;
    const auto rbsp = RemoveEmulationPrevention(pps_nal.data() + 1, pps_nal.size() - 1);
    BitReader br(rbsp.data(), rbsp.size());
    uint32_t pps_id = 0, sps_id = 0, cabac = 0, bfpo = 0, slice_groups_m1 = 0;
    if (!br.ReadUe(&pps_id) || !br.ReadUe(&sps_id) || !br.ReadBits(1, &cabac) ||
        !br.ReadBits(1, &bfpo) || !br.ReadUe(&slice_groups_m1) || slice_groups_m1 != 0) {
        return false;
    }
    uint32_t l0 = 0, l1 = 0, wp = 0, wbi = 0, db = 0, cip = 0, rpc = 0;
    int32_t init_qp = 0, init_qs = 0, cqp = 0;
    if (!br.ReadUe(&l0) || !br.ReadUe(&l1) || !br.ReadBits(1, &wp) || !br.ReadBits(2, &wbi) ||
        !br.ReadSe(&init_qp) || !br.ReadSe(&init_qs) || !br.ReadSe(&cqp) ||
        !br.ReadBits(1, &db) || !br.ReadBits(1, &cip) || !br.ReadBits(1, &rpc)) {
        return false;
    }
    out->valid = true;
    out->entropy_coding_mode_flag = (cabac != 0);
    out->bottom_field_pic_order_in_frame_present_flag = (bfpo != 0);
    out->weighted_pred_flag = (wp != 0);
    out->deblocking_filter_control_present_flag = (db != 0);
    out->redundant_pic_cnt_present_flag = (rpc != 0);
    return true;
}

// Rewrites SPS level_idc >= 60, max_num_ref_frames >= 16, and VUI bitstream_restriction
// (max_num_reorder_frames = 0, max_dec_frame_buffering = 16) so hardware decoders such as
// Qualcomm qcom-iris hold Apple's 9 LTRP refs without buffering 12+ frames for B-frame reorder.
inline std::vector<uint8_t> PatchH264SpsDpb(const std::vector<uint8_t> &sps_nal, H264SpsInfo *before = nullptr) {
    if (sps_nal.size() < 4 || (sps_nal[0] & 0x1f) != 7) return sps_nal;
    auto rbsp = RemoveEmulationPrevention(sps_nal.data() + 1, sps_nal.size() - 1);
    H264SpsInfo info{};
    if (!InspectH264SpsRbsp(rbsp, &info)) return sps_nal;
    if (before) *before = info;
    const uint32_t target_refs = std::max<uint32_t>(16, info.max_num_ref_frames);
    if (info.level_idc >= 60 && info.max_num_ref_frames >= target_refs &&
        info.has_bitstream_restriction && info.max_num_reorder_frames == 0 &&
        info.max_dec_frame_buffering >= target_refs) {
        return sps_nal;
    }

    if (rbsp[2] < 60) rbsp[2] = 60;  // level_idc is RBSP byte 2
    BitWriter bw;
    bw.CopyBitRange(rbsp.data(), 0, info.ref_start_bit);
    bw.WriteUe(target_refs);
    if (info.vui_present) {
        bw.CopyBitRange(rbsp.data(), info.ref_end_bit, info.br_flag_bit);
    } else {
        bw.CopyBitRange(rbsp.data(), info.ref_end_bit, info.vui_flag_bit);
        bw.WriteBit(1);       // vui_parameters_present_flag = 1
        bw.WriteBits(0, 8);   // aspect_ratio..pic_struct flags = 0
    }
    bw.WriteBit(1);           // bitstream_restriction_flag = 1
    bw.WriteBit(1);           // motion_vectors_over_pic_boundaries_flag = 1
    bw.WriteUe(0);            // max_bytes_per_pic_denom = 0
    bw.WriteUe(0);            // max_bits_per_mb_denom = 0
    bw.WriteUe(16);           // log2_max_mv_length_horizontal = 16
    bw.WriteUe(16);           // log2_max_mv_length_vertical = 16
    bw.WriteUe(0);            // max_num_reorder_frames = 0 (zero reorder delay!)
    bw.WriteUe(target_refs);  // max_dec_frame_buffering = 16
    bw.WriteBit(1);           // rbsp_stop_one_bit
    std::vector<uint8_t> out_rbsp = bw.bytes();
    H264SpsInfo check{};
    if (!InspectH264SpsRbsp(out_rbsp, &check)) return sps_nal;

    const auto ebsp = AddEmulationPrevention(out_rbsp);
    std::vector<uint8_t> out;
    out.reserve(1 + ebsp.size());
    out.push_back(sps_nal[0]);
    out.insert(out.end(), ebsp.begin(), ebsp.end());
    return out;
}

// Qualcomm qcom-iris still holds 1 picture in its DPB reorder queue whenever pic_order_cnt_type == 0
// (~17 ms extra latency at 60 fps, ~200 ms on static screens), even when max_num_reorder_frames == 0.
// Because Apple's CABAC screen stream uses only I and P slices (where RefPicList0 and MMCO depend only
// on frame_num/LongTermPicNum, never PicOrderCnt) and CABAC aligns slice_data() to a byte boundary after
// cabac_alignment_one_bit, rewriting SPS pic_order_cnt_type 0 -> 2 and stripping pic_order_cnt_lsb from
// each slice header produces bit-exact decoded output with 0-frame delay (~2 ms hardware decode).
inline std::vector<uint8_t> PatchH264SpsPoc2(const std::vector<uint8_t> &sps_nal, const H264PpsInfo &pps,
                                             H264SpsInfo *raw_before = nullptr, bool *rewrote_poc2 = nullptr) {
    if (rewrote_poc2) *rewrote_poc2 = false;
    std::vector<uint8_t> dpb_sps = PatchH264SpsDpb(sps_nal, raw_before);
    if (!pps.valid || !pps.entropy_coding_mode_flag || pps.weighted_pred_flag) return dpb_sps;

    const auto rbsp = RemoveEmulationPrevention(dpb_sps.data() + 1, dpb_sps.size() - 1);
    H264SpsInfo info{};
    if (!InspectH264SpsRbsp(rbsp, &info)) return dpb_sps;
    if (info.pic_order_cnt_type != 0 || !info.frame_mbs_only_flag || info.separate_colour_plane_flag) return dpb_sps;

    BitWriter bw;
    bw.CopyBitRange(rbsp.data(), 0, info.poc_start_bit);
    bw.WriteUe(2);  // pic_order_cnt_type = 2
    if (!bw.CopyRemainingRbspWithRealignedStopBit(rbsp.data(), rbsp.size(), info.poc_end_bit)) return dpb_sps;

    H264SpsInfo check{};
    if (!InspectH264SpsRbsp(bw.bytes(), &check) || check.pic_order_cnt_type != 2) return dpb_sps;

    const auto ebsp = AddEmulationPrevention(bw.bytes());
    std::vector<uint8_t> out;
    out.reserve(1 + ebsp.size());
    out.push_back(dpb_sps[0]);
    out.insert(out.end(), ebsp.begin(), ebsp.end());
    if (rewrote_poc2) *rewrote_poc2 = true;
    return out;
}

// Resolve visible frame size when the V4L2 decoder reports macroblock-aligned dimensions
// (for example 3456x976 for a 3456x972 display because 972 is not a multiple of 16) while
// either the H.264 SPS frame_cropping or the negotiated backing framebuffer gives the true
// visible size. Only trims within one H.264 macroblock (< 16 px) so a stale display hint
// during a mid-stream resolution switch cannot crop a genuinely different decoder frame.
inline void ResolveVisibleCrop(uint32_t dec_w, uint32_t dec_h,
                               uint32_t sps_w, uint32_t sps_h,
                               uint32_t hint_w, uint32_t hint_h,
                               uint32_t *out_w, uint32_t *out_h) {
    auto pick = [](uint32_t dec, uint32_t sps, uint32_t hint) -> uint32_t {
        if (sps > 0 && sps < dec && (dec - sps) < 16) return sps;
        if (hint > 0 && hint < dec && (dec - hint) < 16) return hint;
        return dec;
    };
    if (out_w) *out_w = pick(dec_w, sps_w, hint_w);
    if (out_h) *out_h = pick(dec_h, sps_h, hint_h);
}

inline std::vector<uint8_t> PatchH264SliceHeaderPoc2(const std::vector<uint8_t> &nal,
                                                     const H264SpsInfo &orig_sps,
                                                     const H264PpsInfo &pps) {
    if (nal.size() < 4) return nal;
    const uint8_t hdr = nal[0];
    const uint8_t t = hdr & 0x1f;
    const uint8_t nri = (hdr >> 5) & 0x03;
    if ((t != 1 && t != 5) || orig_sps.pic_order_cnt_type != 0 || !orig_sps.frame_mbs_only_flag ||
        orig_sps.separate_colour_plane_flag || !pps.valid || !pps.entropy_coding_mode_flag || pps.weighted_pred_flag) {
        return nal;
    }

    const size_t scan_end = std::min<size_t>(nal.size(), 97);
    std::vector<uint8_t> rbsp;
    std::vector<size_t> rbsp_to_ebsp;
    rbsp.reserve(96);
    rbsp_to_ebsp.reserve(96);
    size_t zeros = 0;
    for (size_t i = 1; i < scan_end; ++i) {
        const uint8_t b = nal[i];
        if (zeros == 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(b);
        rbsp_to_ebsp.push_back(i);
        zeros = (b == 0x00) ? zeros + 1 : 0;
    }

    BitReader br(rbsp.data(), rbsp.size());
    uint32_t first_mb = 0, slice_type = 0, pps_id = 0, fn = 0;
    if (!br.ReadUe(&first_mb) || !br.ReadUe(&slice_type) || !br.ReadUe(&pps_id)) return nal;
    const uint32_t st = slice_type % 5;
    if (st != 0 && st != 2) return nal;  // Only P (0) and I (2) slices
    if (!br.ReadBits(orig_sps.log2_max_frame_num_minus4 + 4, &fn)) return nal;
    if (t == 5) {
        uint32_t idr_id = 0;
        if (!br.ReadUe(&idr_id)) return nal;
    }

    const size_t poc_lsb_start = br.pos();
    uint32_t poc_lsb = 0;
    if (!br.ReadBits(orig_sps.log2_max_pic_order_cnt_lsb_minus4 + 4, &poc_lsb)) return nal;
    if (pps.bottom_field_pic_order_in_frame_present_flag) {
        int32_t delta_bot = 0;
        if (!br.ReadSe(&delta_bot)) return nal;
    }
    const size_t poc_lsb_end = br.pos();

    if (pps.redundant_pic_cnt_present_flag) {
        uint32_t rpc = 0;
        if (!br.ReadUe(&rpc)) return nal;
    }
    if (st == 0) {
        uint32_t override_flag = 0, rplm_flag = 0;
        if (!br.ReadBits(1, &override_flag)) return nal;
        if (override_flag) {
            uint32_t l0_m1 = 0;
            if (!br.ReadUe(&l0_m1)) return nal;
        }
        if (!br.ReadBits(1, &rplm_flag)) return nal;
        if (rplm_flag) {
            bool ended = false;
            for (int k = 0; k < 32; ++k) {
                uint32_t op = 0, val = 0;
                if (!br.ReadUe(&op)) return nal;
                if (op == 3) {
                    ended = true;
                    break;
                }
                if (op > 2 || !br.ReadUe(&val)) return nal;
            }
            if (!ended) return nal;
        }
    }
    if (nri != 0) {
        if (t == 5) {
            uint32_t drpm_idr = 0;
            if (!br.ReadBits(2, &drpm_idr)) return nal;
        } else {
            uint32_t adaptive = 0;
            if (!br.ReadBits(1, &adaptive)) return nal;
            if (adaptive) {
                bool ended = false;
                for (int k = 0; k < 32; ++k) {
                    uint32_t mmco = 0, a = 0, b = 0;
                    if (!br.ReadUe(&mmco)) return nal;
                    if (mmco == 0) {
                        ended = true;
                        break;
                    }
                    if (mmco > 6) return nal;
                    if (mmco != 5 && !br.ReadUe(&a)) return nal;
                    if (mmco == 3 && !br.ReadUe(&b)) return nal;
                }
                if (!ended) return nal;
            }
        }
    }
    if (st == 0) {
        uint32_t cabac_idc = 0;
        if (!br.ReadUe(&cabac_idc)) return nal;
    }
    int32_t qp_delta = 0;
    if (!br.ReadSe(&qp_delta)) return nal;
    if (pps.deblocking_filter_control_present_flag) {
        uint32_t dis_idc = 0;
        int32_t alpha = 0, beta = 0;
        if (!br.ReadUe(&dis_idc)) return nal;
        if (dis_idc != 1 && (!br.ReadSe(&alpha) || !br.ReadSe(&beta))) return nal;
    }

    const size_t align_start = br.pos();
    while ((br.pos() & 7) != 0) {
        uint32_t one = 0;
        if (!br.ReadBits(1, &one) || one != 1) return nal;
    }
    const size_t cabac_rbsp_idx = br.pos() >> 3;
    if (cabac_rbsp_idx > rbsp.size()) return nal;

    BitWriter bw;
    bw.CopyBitRange(rbsp.data(), 0, poc_lsb_start);
    bw.CopyBitRange(rbsp.data(), poc_lsb_end, align_start);
    size_t hdr_bits = poc_lsb_start + (align_start - poc_lsb_end);
    while ((hdr_bits & 7) != 0) {
        bw.WriteBit(1);  // cabac_alignment_one_bit
        ++hdr_bits;
    }

    // Advance into CABAC payload until a non-zero byte resets the EBSP zero-run state (zeros == 0)
    // in both streams so the remaining nal suffix can be copied directly without unescaping.
    size_t sync_rbsp = cabac_rbsp_idx;
    while (sync_rbsp < rbsp.size()) {
        ++sync_rbsp;
        if (rbsp[sync_rbsp - 1] != 0x00) break;
    }

    if (scan_end == nal.size() && sync_rbsp == rbsp.size()) {
        bw.CopyBitRange(rbsp.data(), cabac_rbsp_idx * 8, rbsp.size() * 8);
        const auto ebsp = AddEmulationPrevention(bw.bytes());
        std::vector<uint8_t> out;
        out.reserve(1 + ebsp.size());
        out.push_back(hdr);
        out.insert(out.end(), ebsp.begin(), ebsp.end());
        return out;
    }

    if (sync_rbsp < rbsp.size() && rbsp[sync_rbsp - 1] != 0x00) {
        bw.CopyBitRange(rbsp.data(), cabac_rbsp_idx * 8, sync_rbsp * 8);
        const auto prefix_ebsp = AddEmulationPrevention(bw.bytes());
        const size_t tail_ebsp_off = rbsp_to_ebsp[sync_rbsp];
        std::vector<uint8_t> out;
        out.reserve(1 + prefix_ebsp.size() + (nal.size() - tail_ebsp_off));
        out.push_back(hdr);
        out.insert(out.end(), prefix_ebsp.begin(), prefix_ebsp.end());
        out.insert(out.end(), nal.begin() + ssize_t(tail_ebsp_off), nal.end());
        return out;
    }

    // Fallback when >= 64 leading CABAC bytes are all 0x00: unescape full NAL.
    const auto full_rbsp = RemoveEmulationPrevention(nal.data() + 1, nal.size() - 1);
    bw.CopyBitRange(full_rbsp.data(), cabac_rbsp_idx * 8, full_rbsp.size() * 8);
    const auto ebsp = AddEmulationPrevention(bw.bytes());
    std::vector<uint8_t> out;
    out.reserve(1 + ebsp.size());
    out.push_back(hdr);
    out.insert(out.end(), ebsp.begin(), ebsp.end());
    return out;
}

inline bool ParseAvcConfig0x92(const uint8_t *pay, size_t len,
                               std::vector<uint8_t> *sps_out, std::vector<uint8_t> *pps_out) {
    if (len < 16 || pay[0] != kAppleAvcConfigMarker) return false;
    auto find4 = [&](const char tag[4], size_t start) -> ssize_t {
        for (size_t i = start; i + 4 <= len; ++i) {
            if (std::memcmp(pay + i, tag, 4) == 0) return ssize_t(i);
        }
        return -1;
    };
    const ssize_t avc1 = find4("avc1", 0);
    const ssize_t avcc = find4("avcC", avc1 >= 0 ? size_t(avc1) : 0);
    if (avcc < 0) return false;
    size_t p = size_t(avcc) + 4;
    if (p + 6 > len) return false;
    const uint8_t num_sps = pay[p + 5] & 0x1f;
    p += 6;
    sps_out->clear();
    for (uint8_t i = 0; i < num_sps; ++i) {
        if (p + 2 > len) return false;
        const uint16_t slen = ReadBe16(pay + p);
        p += 2;
        if (slen == 0 || p + slen > len) return false;
        if (sps_out->empty()) sps_out->assign(pay + p, pay + p + slen);
        p += slen;
    }
    if (p + 1 > len) return false;
    const uint8_t num_pps = pay[p++];
    pps_out->clear();
    for (uint8_t i = 0; i < num_pps; ++i) {
        if (p + 2 > len) return false;
        const uint16_t plen = ReadBe16(pay + p);
        p += 2;
        if (plen == 0 || p + plen > len) return false;
        if (pps_out->empty()) pps_out->assign(pay + p, pay + p + plen);
        p += plen;
    }
    return !sps_out->empty() && !pps_out->empty();
}

inline bool IsH264IntraNal(const uint8_t *nal, size_t len) {
    if (len < 2) return false;
    const uint8_t t = nal[0] & 0x1f;
    if (t == 5) return true;
    if (t == 1) {
        const auto rbsp = RemoveEmulationPrevention(nal + 1, std::min<size_t>(16, len - 1));
        BitReader br(rbsp.data(), rbsp.size());
        uint32_t first_mb = 0, slice_type = 0;
        if (br.ReadUe(&first_mb) && br.ReadUe(&slice_type)) {
            return slice_type == 2 || slice_type == 7;
        }
    }
    return false;
}

class H264RtpAssembler {
public:
    struct AccessUnit {
        uint32_t ssrc = 0;
        uint32_t rtp_timestamp = 0;
        bool is_keyframe = false;
        bool config_updated = false;
        std::vector<uint8_t> annex_b;
    };

    bool has_config() const { return !patched_sps_.empty() && !pps_.empty(); }
    const H264SpsInfo &raw_sps_info() const { return raw_sps_info_; }
    bool poc2_rewritten() const { return poc2_rewritten_; }
    uint64_t lost_packets() const { return lost_packets_; }

    std::set<uint16_t> TakePendingNacks(uint32_t *out_ssrc) {
        for (auto &kv : pending_nacks_) {
            if (!kv.second.empty()) {
                *out_ssrc = kv.first;
                auto res = std::move(kv.second);
                kv.second.clear();
                return res;
            }
        }
        return {};
    }

    // Feeds one decrypted RTP packet; emits 0 or more reassembled Annex-B access units.
    void PushPacket(const SrtpContext::RtpHeader &hdr, const std::vector<uint8_t> &payload,
                    std::vector<AccessUnit> *out_aus) {
        if (payload.empty()) return;

        // Track sequence continuity per SSRC.
        auto it_seq = last_seq_.find(hdr.ssrc);
        if (it_seq != last_seq_.end()) {
            const uint16_t gap = uint16_t(hdr.seq - it_seq->second - 1);
            if (gap > 0 && gap < 256) {
                lost_packets_ += gap;
                for (uint16_t s = uint16_t(it_seq->second + 1); s != hdr.seq; ++s) {
                    pending_nacks_[hdr.ssrc].insert(s);
                }
            }
        }
        if (it_seq == last_seq_.end() || int16_t(hdr.seq - it_seq->second) > 0) {
            last_seq_[hdr.ssrc] = hdr.seq;
        }

        // Apple H.264 decoder configuration packet (0x92 avc1/avcC).
        if (payload[0] == kAppleAvcConfigMarker) {
            std::vector<uint8_t> sps, pps;
            if (ParseAvcConfig0x92(payload.data(), payload.size(), &sps, &pps)) {
                raw_sps_ = sps;
                pps_ = std::move(pps);
                InspectH264Pps(pps_, &pps_info_);
                patched_sps_ = PatchH264SpsPoc2(raw_sps_, pps_info_, &raw_sps_info_, &poc2_rewritten_);
                inject_config_next_ = true;
            }
            return;
        }

        // Flush any older timestamp group on the same SSRC before buffering a new timestamp.
        auto &grp = groups_[hdr.ssrc];
        if (!grp.packets.empty() && grp.timestamp != hdr.timestamp) {
            FlushGroup(hdr.ssrc, grp, out_aus);
        }
        grp.timestamp = hdr.timestamp;
        grp.packets[hdr.seq] = payload;
        if (hdr.marker) {
            FlushGroup(hdr.ssrc, grp, out_aus);
        }
    }

private:
    struct Group {
        uint32_t timestamp = 0;
        std::map<uint16_t, std::vector<uint8_t>> packets;
    };

    void FlushGroup(uint32_t ssrc, Group &grp, std::vector<AccessUnit> *out_aus) {
        if (grp.packets.empty()) return;

        std::vector<std::vector<uint8_t>> nals;
        std::vector<uint8_t> fu_buf;
        bool fu_active = false;
        uint16_t prev_seq = 0;
        bool have_prev_seq = false;

        for (const auto &kv : grp.packets) {
            const uint16_t seq = kv.first;
            const auto &pay = kv.second;
            if (have_prev_seq && uint16_t(seq - prev_seq) != 1) {
                fu_active = false;
                fu_buf.clear();
            }
            have_prev_seq = true;
            prev_seq = seq;

            if (pay.empty() || pay[0] == kAppleAvcConfigMarker) continue;
            const uint8_t b0 = pay[0];
            const uint8_t t = b0 & 0x1f;

            if (t == 28 || t == 29) {  // FU-A (DON-less) or FU-B (2B DON)
                const size_t off = (t == 29) ? 4 : 2;
                if (pay.size() < off) continue;
                const uint8_t fu_hdr = pay[1];
                const bool start = (fu_hdr & 0x80) != 0;
                const bool end = (fu_hdr & 0x40) != 0;
                const uint8_t inner_type = fu_hdr & 0x1f;
                if (start) {
                    fu_buf.clear();
                    fu_buf.push_back(uint8_t((b0 & 0xe0) | inner_type));
                    fu_buf.insert(fu_buf.end(), pay.begin() + ssize_t(off), pay.end());
                    fu_active = true;
                    if (end) {
                        nals.push_back(std::move(fu_buf));
                        fu_buf.clear();
                        fu_active = false;
                    }
                } else if (fu_active) {
                    fu_buf.insert(fu_buf.end(), pay.begin() + ssize_t(off), pay.end());
                    if (end) {
                        nals.push_back(std::move(fu_buf));
                        fu_buf.clear();
                        fu_active = false;
                    }
                }
            } else if (t == 24) {  // STAP-A (Apple includes 2B DON after 1B indicator)
                size_t pos = 3;
                while (pos + 2 <= pay.size()) {
                    const uint16_t sz = ReadBe16(pay.data() + pos);
                    pos += 2;
                    if (sz == 0 || pos + sz > pay.size()) break;
                    nals.emplace_back(pay.begin() + ssize_t(pos), pay.begin() + ssize_t(pos + sz));
                    pos += sz;
                }
            } else if (t >= 1 && t <= 23) {  // DON-less Single NAL
                if (pay.size() >= 2) nals.push_back(pay);
            }
        }
        grp.packets.clear();
        if (nals.empty()) return;

        bool has_inband_sps = false;
        bool is_key = false;
        for (const auto &nal : nals) {
            if ((nal[0] & 0x1f) == 8) {
                pps_ = nal;
                InspectH264Pps(pps_, &pps_info_);
                if (!raw_sps_.empty()) {
                    patched_sps_ = PatchH264SpsPoc2(raw_sps_, pps_info_, &raw_sps_info_, &poc2_rewritten_);
                }
            }
        }
        for (auto &nal : nals) {
            const uint8_t t = nal[0] & 0x1f;
            if (t == 7) {
                has_inband_sps = true;
                raw_sps_ = nal;
                nal = PatchH264SpsPoc2(raw_sps_, pps_info_, &raw_sps_info_, &poc2_rewritten_);
                patched_sps_ = nal;
            }
            if (IsH264IntraNal(nal.data(), nal.size())) is_key = true;
            if ((t == 1 || t == 5) && poc2_rewritten_) {
                nal = PatchH264SliceHeaderPoc2(nal, raw_sps_info_, pps_info_);
            }
        }

        if (!has_config() && !has_inband_sps) return;  // Can't decode before first SPS/PPS.

        AccessUnit au;
        au.ssrc = ssrc;
        au.rtp_timestamp = grp.timestamp;
        au.is_keyframe = is_key;
        au.config_updated = inject_config_next_;

        auto append_nal = [&](const std::vector<uint8_t> &nal) {
            static constexpr uint8_t kStart[4] = {0x00, 0x00, 0x00, 0x01};
            au.annex_b.insert(au.annex_b.end(), kStart, kStart + 4);
            au.annex_b.insert(au.annex_b.end(), nal.begin(), nal.end());
        };

        if ((inject_config_next_ || is_key) && !has_inband_sps && has_config()) {
            append_nal(patched_sps_);
            append_nal(pps_);
            inject_config_next_ = false;
        }
        for (const auto &nal : nals) append_nal(nal);
        // Append a trailing 6-byte Access Unit Delimiter (NAL 9) so stateful V4L2 H.264
        // decoders (Qualcomm qcom-iris) flush the picture immediately without waiting for
        // the next frame's start code. Combined with poc_type 0 -> 2, decode delay is ~2 ms (0 inside).
        append_nal({0x09, 0xf0});
        out_aus->push_back(std::move(au));
    }

    std::vector<uint8_t> raw_sps_;
    std::vector<uint8_t> patched_sps_;
    std::vector<uint8_t> pps_;
    H264SpsInfo raw_sps_info_{};
    H264PpsInfo pps_info_{};
    bool poc2_rewritten_ = false;
    bool inject_config_next_ = false;
    std::map<uint32_t, Group> groups_;
    std::map<uint32_t, uint16_t> last_seq_;
    std::map<uint32_t, std::set<uint16_t>> pending_nacks_;
    uint64_t lost_packets_ = 0;
};

// ── Socket & Live Session Client ─────────────────────────────────────────────

enum class AuthMode { kAuto, kSrp, kNonSrp };

struct SessionOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 5900;
    std::string bind_ip;         // Optional local IPv4 bind for TCP & UDP
    uint16_t udp_ctrl_port = 0;  // 0 => port (5900)
    uint16_t udp_video_port = 0; // 0 => port + 1 (5901)
    std::string username;
    std::string password;
    AuthMode auth_mode = AuthMode::kAuto;
    bool virtual_display = true; // true = send 0x1d virtual display; false = mirror physical screen
    uint32_t logical_w = 1920;
    uint32_t logical_h = 1080;
    double hidpi_scale = 2.0;
    int fps = 60;
    bool audio_enabled = false;
    bool hdr = false;
    bool bake_cursor = true;
    bool warmup_tcp = true;
    int warmup_ms = 1400;
    int settle_ms = 100;
    int toggle_settle_ms = 200;
    KeyboardMappingConfig keyboard{};
};

// Maps a Frametop remote app token ("apple:physical", "apple:virtual", "apple:virtual@1",
// "apple:virtual@2") and requested stream size into Apple Silicon H.264 virtual-display limits
// (<= 4096x2304 backing pixels), preserving aspect ratio when clamping wider ultra-wide sizes
// (e.g. 5120x1440 -> 4096x1152) and keeping 1x scaling when 2x would shrink height below 900 pts.
inline void ResolveAppleDisplayMode(const std::string &app, int req_w, int req_h, SessionOptions *opt) {
    if (app == "apple:physical") {
        opt->virtual_display = false;
        return;
    }
    opt->virtual_display = true;
    double w = std::max(640, req_w);
    double h = std::max(360, req_h);
    if (w > 4096.0 || h > 2304.0) {
        const double s = std::min(4096.0 / w, 2304.0 / h);
        w *= s;
        h *= s;
    }
    const int cw = std::clamp(int(std::lround(w)), 1280, 4096) & ~1;
    const int ch = std::clamp(int(std::lround(h)), 720, 2304) & ~1;
    if (app == "apple:virtual@1") {
        opt->hidpi_scale = 1.0;
        opt->logical_w = uint32_t(cw);
        opt->logical_h = uint32_t(ch);
        return;
    }
    if (app == "apple:virtual@2") {
        opt->hidpi_scale = 2.0;
        if (req_w <= 2048 && req_h <= 1152) {
            opt->logical_w = uint32_t(std::clamp(req_w, 640, 2048) & ~1);
            opt->logical_h = uint32_t(std::clamp(req_h, 360, 1152) & ~1);
        } else {
            opt->logical_w = uint32_t(std::max(640, (cw / 2) & ~1));
            opt->logical_h = uint32_t(std::max(360, (ch / 2) & ~1));
        }
        return;
    }
    // Default "apple:virtual": 4K-tall modes (>= 2560x1800, e.g. 3840x2160 -> 1920x1080@2x) use
    // 2x Retina; shorter or ultra-wide modes (e.g. 4096x1152 32:9, 3440x1440, 2560x1440, 1920x1080)
    // use 1x so macOS keeps >= 1080 vertical points rather than shrinking UI height to 576-720p.
    if (cw >= 2560 && ch >= 1800) {
        opt->hidpi_scale = 2.0;
        opt->logical_w = uint32_t((cw / 2) & ~1);
        opt->logical_h = uint32_t((ch / 2) & ~1);
    } else {
        opt->hidpi_scale = 1.0;
        opt->logical_w = uint32_t(cw);
        opt->logical_h = uint32_t(ch);
    }
}

struct ProbeInfo {
    std::string banner;
    std::vector<uint8_t> security_types;
    int rsa_key_bits = 0;
};

inline bool ResolveIpv4(const std::string &host, in_addr *out_addr, std::string *out_ip = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    const auto *sin = reinterpret_cast<const sockaddr_in *>(res->ai_addr);
    *out_addr = sin->sin_addr;
    if (out_ip) {
        char buf[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf);
        *out_ip = buf;
    }
    freeaddrinfo(res);
    return true;
}

inline int OpenTcpIpv4(const in_addr &dest, uint16_t port, const std::string &bind_ip, int timeout_ms = 10000) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
    if (!bind_ip.empty()) {
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;
        if (inet_pton(AF_INET, bind_ip.c_str(), &local.sin_addr) != 1 ||
            bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof local) < 0) {
            close(fd);
            return -1;
        }
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    peer.sin_addr = dest;
    int r = connect(fd, reinterpret_cast<const sockaddr *>(&peer), sizeof peer);
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (r < 0) {
        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

inline int BindUdpIpv4(const std::string &bind_ip, uint16_t port) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (bind_ip.empty()) {
        local.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_ip.c_str(), &local.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, reinterpret_cast<const sockaddr *>(&local), sizeof local) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

inline bool SendAll(int fd, const uint8_t *buf, size_t len, int timeout_ms = 10000) {
    size_t sent = 0;
    while (sent < len) {
        pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = send(fd, buf + sent, len - sent, 0);
#endif
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += size_t(n);
    }
    return true;
}

inline bool SendAll(int fd, const std::vector<uint8_t> &v, int timeout_ms = 10000) {
    return SendAll(fd, v.data(), v.size(), timeout_ms);
}

inline bool RecvExact(int fd, uint8_t *buf, size_t len, int timeout_ms = 10000) {
    size_t got = 0;
    while (got < len) {
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        const ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        got += size_t(n);
    }
    return true;
}

inline bool DoRfbBannerAndTypes(int fd, std::string *banner_out, std::vector<uint8_t> *types_out) {
    uint8_t banner[12];
    if (!RecvExact(fd, banner, 12, 5000)) return false;
    *banner_out = std::string(reinterpret_cast<const char *>(banner), 12);
    static constexpr uint8_t kVer[12] = {'R', 'F', 'B', ' ', '0', '0', '3', '.', '8', '8', '9', '\n'};
    if (!SendAll(fd, kVer, 12, 5000)) return false;
    uint8_t count = 0;
    if (!RecvExact(fd, &count, 1, 5000) || count == 0) return false;
    types_out->resize(count);
    return RecvExact(fd, types_out->data(), count, 5000);
}

inline UniquePkey Rsa1InitFetchKey(int fd) {
    static constexpr uint8_t kInit[15] = {
        0x21, 0x00, 0x00, 0x00, 0x0a, 0x01, 0x00, 'R', 'S', 'A', '1', 0x00, 0x00, 0x00, 0x00,
    };
    if (!SendAll(fd, kInit, sizeof kInit, 5000)) return nullptr;
    uint8_t len_b[4];
    if (!RecvExact(fd, len_b, 4, 5000)) return nullptr;
    const uint32_t pkt_len = ReadBe32(len_b);
    if (pkt_len < 16 || pkt_len > 16384) return nullptr;
    std::vector<uint8_t> pkt(pkt_len);
    if (!RecvExact(fd, pkt.data(), pkt_len, 5000)) return nullptr;
    const uint32_t key_len = ReadBe32(pkt.data() + 2);
    if (size_t(6) + key_len > pkt.size()) return nullptr;
    return LoadDerPublicKey(pkt.data() + 6, key_len);
}

inline bool ProbeServer(const std::string &host, uint16_t port, const std::string &bind_ip,
                        ProbeInfo *out, std::string *err_out) {
    in_addr dest{};
    if (!ResolveIpv4(host, &dest)) {
        if (err_out) *err_out = "failed to resolve host IPv4";
        return false;
    }
    const int fd = OpenTcpIpv4(dest, port, bind_ip, 5000);
    if (fd < 0) {
        if (err_out) *err_out = "failed to connect TCP";
        return false;
    }
    if (!DoRfbBannerAndTypes(fd, &out->banner, &out->security_types)) {
        close(fd);
        if (err_out) *err_out = "RFB version/security-types handshake failed";
        return false;
    }
    const bool has_33 = std::find(out->security_types.begin(), out->security_types.end(), 33) !=
                        out->security_types.end();
    if (has_33) {
        UniquePkey pk = Rsa1InitFetchKey(fd);
        if (pk) out->rsa_key_bits = EVP_PKEY_get_bits(pk.get());
    }
    close(fd);
    return out->rsa_key_bits > 0 || !has_33;
}

class SessionClient {
public:
    ~SessionClient() { Close(); }

    const NegotiatedCanvas &canvas() const { return canvas_; }
    const DisplayLayout0x451 &layout() const { return layout_; }
    uint16_t server_init_w() const { return server_w_; }
    uint16_t server_init_h() const { return server_h_; }
    const std::string &auth_used() const { return auth_used_; }
    H264RtpAssembler &assembler() { return assembler_; }
    uint64_t video_rtp_packets() const { return video_rtp_pkts_; }
    uint64_t video_rtp_bytes() const { return video_rtp_bytes_; }
    uint64_t audio_rtp_packets() const { return audio_rtp_pkts_; }

    // ScreensharingAgent's PostMouseEvent calls hidpi_ConvertActualLocationsToVirtual, which
    // expects backing ("Actual") pixel coordinates (e.g. 3840x2160 on a 1920x1080@2x Retina
    // display) and divides by the HiDPI scale factor on the Mac host.
    uint16_t backing_pointer_w() const {
        if (layout_.backing_w) return layout_.backing_w;
        if (canvas_.width) return uint16_t(std::min<uint32_t>(canvas_.width, 32767));
        return server_w_;
    }
    uint16_t backing_pointer_h() const {
        if (layout_.backing_h) return layout_.backing_h;
        if (canvas_.height) return uint16_t(std::min<uint32_t>(canvas_.height, 32767));
        return server_h_;
    }

    // Verifies RSA1/SRP-6a (or non-SRP) credentials against screensharingd without mounting
    // a virtual display or starting UDP media streaming.
    bool VerifyCredentials(const SessionOptions &opt, std::string *err_out) {
        Close();
        opt_ = opt;
        if (!ResolveIpv4(opt.host, &dest_addr_, &dest_ip_)) {
            if (err_out) *err_out = "cannot resolve host IPv4: " + opt.host;
            return false;
        }
        std::array<uint8_t, 16> wrap_key{};
        const bool ok = RunAuthWithFallback(opt, &wrap_key, err_out);
        OPENSSL_cleanse(wrap_key.data(), wrap_key.size());
        Close();
        return ok;
    }

    void Close() {
        if (tcp_fd_ >= 0) close(tcp_fd_), tcp_fd_ = -1;
        if (udp_ctrl_fd_ >= 0) close(udp_ctrl_fd_), udp_ctrl_fd_ = -1;
        if (udp_video_fd_ >= 0) close(udp_video_fd_), udp_video_fd_ = -1;
        buttons_mask_ = 0;
    }

    bool Connect(const SessionOptions &opt, std::string *err_out) {
        Close();
        opt_ = opt;
        key_mapper_ = EvdevKeyMapper(opt.keyboard);
        if (!ResolveIpv4(opt.host, &dest_addr_, &dest_ip_)) {
            if (err_out) *err_out = "cannot resolve host IPv4: " + opt.host;
            return false;
        }

        const uint16_t ctrl_port = opt.udp_ctrl_port ? opt.udp_ctrl_port : opt.port;
        const uint16_t video_port = opt.udp_video_port ? opt.udp_video_port : uint16_t(opt.port + 1);
        dest_ctrl_sin_.sin_family = AF_INET;
        dest_ctrl_sin_.sin_port = htons(ctrl_port);
        dest_ctrl_sin_.sin_addr = dest_addr_;
        dest_video_sin_.sin_family = AF_INET;
        dest_video_sin_.sin_port = htons(video_port);
        dest_video_sin_.sin_addr = dest_addr_;

        // Bind UDP media sockets before negotiating 0x1c so the initial burst isn't dropped.
        udp_ctrl_fd_ = BindUdpIpv4(opt.bind_ip, ctrl_port);
        udp_video_fd_ = BindUdpIpv4(opt.bind_ip, video_port);
        if (udp_ctrl_fd_ < 0 || udp_video_fd_ < 0) {
            if (err_out) {
                *err_out = "failed to bind local UDP ports " + std::to_string(ctrl_port) + "/" +
                           std::to_string(video_port) +
                           " (if testing on the same Mac, pass --bind-ip with a secondary loopback/LAN IP)";
            }
            Close();
            return false;
        }

        if (opt.warmup_tcp) {
            const int wfd = OpenTcpIpv4(dest_addr_, opt.port, opt.bind_ip, 5000);
            if (wfd >= 0) {
                std::string b;
                std::vector<uint8_t> t;
                DoRfbBannerAndTypes(wfd, &b, &t);
                shutdown(wfd, SHUT_RDWR);
                close(wfd);
                if (opt.warmup_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opt.warmup_ms));
            }
        }

        std::array<uint8_t, 16> wrap_key{};
        if (!RunAuthWithFallback(opt, &wrap_key, err_out)) {
            Close();
            return false;
        }

        // ClientInit (0xC1) + ServerInit (24B + name).
        const uint8_t client_init = 0xc1;
        uint8_t sinit[24];
        if (!SendAll(tcp_fd_, &client_init, 1) || !RecvExact(tcp_fd_, sinit, 24)) {
            if (err_out) *err_out = "ClientInit/ServerInit failed";
            Close();
            return false;
        }
        server_w_ = ReadBe16(sinit);
        server_h_ = ReadBe16(sinit + 2);
        const uint32_t name_len = ReadBe32(sinit + 20);
        if (name_len > 0) {
            std::vector<uint8_t> name_buf(std::min<uint32_t>(name_len, 4096));
            if (name_len > 4096 || !RecvExact(tcp_fd_, name_buf.data(), name_len)) {
                if (err_out) *err_out = "ServerInit name read failed";
                Close();
                return false;
            }
        }

        // Check optional SessionSelect prompt (00 4a 00 01) when auth user != console user.
        if (!HandleOptionalSessionSelect(opt, wrap_key.data(), err_out)) {
            Close();
            return false;
        }

        // Plaintext Apple prelude + optional virtual display (0x1d) + SetEncodings (0x02).
        auto prelude = BuildViewerInfo();
        const auto f12 = Build0x12Followup();
        prelude.insert(prelude.end(), f12.begin(), f12.end());
        if (!SendAll(tcp_fd_, prelude)) {
            if (err_out) *err_out = "ViewerInfo send failed";
            Close();
            return false;
        }
        if (opt.settle_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opt.settle_ms));

        if (opt.virtual_display &&
            !SendAll(tcp_fd_, BuildVirtualDisplay0x1d(opt.logical_w, opt.logical_h, opt.hidpi_scale, opt.hdr,
                                                      "Frametop Virtual Display", double(opt.fps)))) {
            if (err_out) *err_out = "SetDisplayConfiguration (0x1d) send failed";
            Close();
            return false;
        }
        if (!SendAll(tcp_fd_, BuildSetEncodings())) {
            if (err_out) *err_out = "SetEncodings send failed";
            Close();
            return false;
        }

        // Drain cleartext server FramebufferUpdate rectangles until 0x44f (1103) rekey arrives.
        if (!ReadUntilEnc1103(wrap_key.data(), err_out)) {
            Close();
            return false;
        }
        OPENSSL_cleanse(wrap_key.data(), wrap_key.size());

        if (!SendAll(tcp_fd_, BuildPostEncryptionToggle())) {
            if (err_out) *err_out = "PostEncryptionToggle send failed";
            Close();
            return false;
        }
        if (opt.toggle_settle_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opt.toggle_settle_ms));
        DrainEncryptedControl(250);

        // Re-assert 0x1d virtual display over the encrypted channel once ScreensharingAgent's
        // Mach port is attached so cold-booted sessions mount SLVirtualDisplay before 0x1c.
        if (opt.virtual_display) {
            if (!SendEncrypted(BuildVirtualDisplay0x1d(opt.logical_w, opt.logical_h, opt.hidpi_scale, opt.hdr,
                                                       "Frametop Virtual Display", double(opt.fps)))) {
                if (err_out) *err_out = "encrypted SetDisplayConfiguration (0x1d) send failed";
                Close();
                return false;
            }
            DrainEncryptedControl(opt.toggle_settle_ms > 50 ? 450 : 20);
        }

        // Build & send encrypted SetEncodings + 0x1c MediaStreamOptions offer.
        OfferConfig ocfg;
        ocfg.codec = VideoCodec::kAvc;
        ocfg.tiles_per_frame = 1;
        ocfg.ltrp = false;
        ocfg.audio_enabled = opt.audio_enabled;
        if (!CreateNegotiatorOffers(ocfg, &offers_) || !NegotiationKeys::Generate(&keys_)) {
            if (err_out) *err_out = "failed to generate 0x1c media offers";
            Close();
            return false;
        }
        if (!video_rx_srtp_.InitFromBlob(keys_.video_key_s.data()) ||
            !video_tx_srtp_.InitFromBlob(keys_.video_key_v.data()) ||
            !audio_rx_srtp_.InitFromBlob(keys_.audio_key_s.data()) ||
            !audio_tx_srtp_.InitFromBlob(keys_.audio_key_v.data())) {
            if (err_out) *err_out = "SRTP context initialization failed";
            Close();
            return false;
        }

        if (!SendEncrypted(BuildSetEncodings())) {
            if (err_out) *err_out = "encrypted SetEncodings send failed";
            Close();
            return false;
        }
        const auto msg_1c = Build0x1cMessage(offers_.audio_bplist, offers_.video_bplist, keys_, opt.bake_cursor,
                                             opt.fps >= 45);
        if (!SendEncrypted(msg_1c)) {
            if (err_out) *err_out = "encrypted 0x1c send failed";
            Close();
            return false;
        }
        if (!opt.bake_cursor) SendEncrypted(BuildFbuRequest(false));

        // Punch UDP paths early and await 0x1c non-zero canvas answer (with degenerate retries).
        SendRtcpKeepalive(true);
        if (!WaitVideoAnswer(msg_1c, err_out)) {
            Close();
            return false;
        }

        fcntl(tcp_fd_, F_SETFL, fcntl(tcp_fd_, F_GETFL, 0) | O_NONBLOCK);
        last_tx_pulse_ns_ = NowNs();
        SendRtcpKeepalive(true);
        return true;
    }

    // Non-blocking poll cycle: drains UDP video/ctrl, sends periodic SRTCP RR/SR + NACKs, drains TCP control.
    // Optional extra_poll_fd (e.g. V4L2 decoder fd) wakes poll() immediately when hardware decode completes.
    bool PollStep(int timeout_ms, std::vector<H264RtpAssembler::AccessUnit> *out_aus,
                  std::string *err_out = nullptr, int extra_poll_fd = -1,
                  std::vector<std::vector<uint8_t>> *out_audio_aus = nullptr) {
        if (tcp_fd_ < 0 || udp_video_fd_ < 0) return false;
        pollfd pfds[4] = {
            {udp_video_fd_, POLLIN, 0},
            {udp_ctrl_fd_, POLLIN, 0},
            {tcp_fd_, POLLIN, 0},
            {extra_poll_fd, short(extra_poll_fd >= 0 ? (POLLIN | POLLPRI) : 0), 0},
        };
        poll(pfds, extra_poll_fd >= 0 ? 4 : 3, timeout_ms);

        // 1. Drain UDP video + muxed video RTCP.
        uint8_t udp_buf[2048];
        for (int n = 0; n < 256; ++n) {
            const ssize_t r = recv(udp_video_fd_, udp_buf, sizeof udp_buf, 0);
            if (r <= 0) break;
            if (r < 12) continue;
            const uint8_t pt_raw = udp_buf[1];
            if (pt_raw >= 192 && pt_raw <= 223) continue;  // SRTCP from server
            SrtpContext::RtpHeader hdr{};
            std::vector<uint8_t> pay;
            if (video_rx_srtp_.DecryptRtp(udp_buf, size_t(r), &hdr, &pay)) {
                ++video_rtp_pkts_;
                video_rtp_bytes_ += size_t(r);
                last_media_ssrc_ = hdr.ssrc;
                assembler_.PushPacket(hdr, pay, out_aus);
            }
        }

        // 2. Drain UDP audio/ctrl socket (decrypting AAC-ELD packets when audio is active).
        for (int n = 0; n < 256; ++n) {
            const ssize_t r = recv(udp_ctrl_fd_, udp_buf, sizeof udp_buf, 0);
            if (r <= 0) break;
            if (!opt_.audio_enabled || !out_audio_aus || r < 12) continue;
            const uint8_t pt_raw = udp_buf[1];
            if (pt_raw >= 192 && pt_raw <= 223) continue;  // SRTCP from server
            SrtpContext::RtpHeader hdr{};
            std::vector<uint8_t> pay;
            if (audio_rx_srtp_.DecryptRtp(udp_buf, size_t(r), &hdr, &pay) && !pay.empty()) {
                ++audio_rtp_pkts_;
                ParseRfc3640AuPayloads(pay.data(), pay.size(), out_audio_aus);
            }
        }

        // 3. Promptly emit SRTCP NACKs when packet gaps occur.
        uint32_t nack_ssrc = 0;
        const auto lost = assembler_.TakePendingNacks(&nack_ssrc);
        if (!lost.empty()) {
            const auto nack = BuildNack(offers_.video_ssrc, nack_ssrc, lost);
            SendVideoSrtcp(nack);
        }

        // 4. Periodic 500ms SRTCP RR/SR keepalive.
        const int64_t now = NowNs();
        if (now - last_tx_pulse_ns_ >= 500'000'000LL) {
            last_tx_pulse_ns_ = now;
            ++pulse_tick_;
            SendRtcpKeepalive((pulse_tick_ % 10) == 0);
        }

        // 5. Drain non-blocking TCP control frames.
        if (pfds[2].revents & (POLLHUP | POLLERR)) {
            if (err_out) *err_out = "server closed TCP control socket";
            return false;
        }
        if (pfds[2].revents & POLLIN) {
            if (!DrainNonBlockingTcp(err_out)) return false;
        }
        return true;
    }

    void RequestKeyframe() {
        if (last_media_ssrc_ != 0) {
            SendVideoSrtcp(BuildFirLegacy(last_media_ssrc_));
            SendVideoSrtcp(BuildPli(offers_.video_ssrc, last_media_ssrc_));
        }
    }

    bool SendPointerMove(double stream_x, double stream_y, uint32_t stream_w, uint32_t stream_h) {
        if (tcp_fd_ < 0) return false;
        last_ptr_x_ = ScalePointerCoord(stream_x, stream_w, backing_pointer_w());
        last_ptr_y_ = ScalePointerCoord(stream_y, stream_h, backing_pointer_h());
        return SendEncrypted(BuildPointerEvent(buttons_mask_, last_ptr_x_, last_ptr_y_));
    }

    bool SendMouseButton(uint32_t linux_btn, bool down) {
        if (tcp_fd_ < 0) return false;
        const uint8_t bit = MouseButtonBit(linux_btn);
        if (bit == 0) return true;
        if (down) buttons_mask_ |= bit;
        else buttons_mask_ = uint8_t(buttons_mask_ & ~bit);
        return SendEncrypted(BuildPointerEvent(buttons_mask_, last_ptr_x_, last_ptr_y_));
    }

    // Wayland/ft-screens scroll convention: dy < 0 is scroll up, dy > 0 is scroll down.
    // In screensharingd's HandlePointerEventMessage, exact button_mask == 0x08 posts scroll up (+1)
    // and exact button_mask == 0x10 posts scroll down (-1) via SSAgent_PostScrollWheelEvent_rpc
    // without touching mouse button state; OR-ing buttons_mask_ or sending 0x20/0x40 falls through
    // into PostMouseEventIntoSession as extra mouse button clicks.
    bool SendMouseScroll(double dx, double dy) {
        (void)dx;
        if (tcp_fd_ < 0 || dy == 0.0) return tcp_fd_ >= 0;
        scroll_accum_y_ = 0.0;
        const int ticks = std::clamp(int(std::lround(std::fabs(dy))), 1, 12);
        const uint8_t bit = (dy < 0.0) ? 0x08 : 0x10;
        bool ok = true;
        for (int t = 0; t < ticks; ++t) {
            ok = SendEncrypted(BuildPointerEvent(bit, last_ptr_x_, last_ptr_y_)) && ok;
        }
        return ok;
    }

    const KeyboardMappingConfig &keyboard_mapping() const { return key_mapper_.config(); }

    bool SendMappedKey(bool down, uint32_t ks) {
        if (tcp_fd_ < 0) return false;
        uint16_t mac_vk = 0;
        if (IsAppleMacKeycodeKeysym(ks, &mac_vk)) {
            std::vector<uint8_t> pkt;
            return BuildAppleKeycodeEvent0x10(cipher_.cbc_key().data(), down, mac_vk, &pkt) &&
                   SendEncrypted(pkt);
        }
        return SendEncrypted(BuildKeyEvent(down, ks));
    }

    bool SetKeyboardMapping(const KeyboardMappingConfig &cfg) {
        if (cfg == key_mapper_.config()) return false;
        opt_.keyboard = cfg;
        key_mapper_.SetConfig(cfg, [&](bool d, uint32_t ks) {
            if (tcp_fd_ >= 0) SendMappedKey(d, ks);
        });
        return true;
    }

    bool SendEvdevKey(uint32_t evdev_code, bool down) {
        if (tcp_fd_ < 0) return false;
        bool ok = true;
        key_mapper_.Process(evdev_code, down, [&](bool d, uint32_t ks) {
            ok = SendMappedKey(d, ks) && ok;
        });
        return ok;
    }

    bool SendInputBlur() {
        if (tcp_fd_ < 0) return false;
        bool ok = true;
        key_mapper_.Blur([&](bool d, uint32_t ks) {
            ok = SendMappedKey(d, ks) && ok;
        });
        if (buttons_mask_ != 0) {
            buttons_mask_ = 0;
            ok = SendEncrypted(BuildPointerEvent(0, last_ptr_x_, last_ptr_y_)) && ok;
        }
        return ok;
    }

private:
    bool RunSingleAuth(bool use_srp, std::array<uint8_t, 16> *wrap_key, std::string *err_out) {
        tcp_fd_ = OpenTcpIpv4(dest_addr_, opt_.port, opt_.bind_ip, 10000);
        if (tcp_fd_ < 0) {
            if (err_out) *err_out = "TCP connect failed to " + dest_ip_;
            return false;
        }
        std::string banner;
        std::vector<uint8_t> sec_types;
        if (!DoRfbBannerAndTypes(tcp_fd_, &banner, &sec_types)) {
            if (err_out) *err_out = "RFB banner/security-types exchange failed";
            return false;
        }
        if (std::find(sec_types.begin(), sec_types.end(), 33) == sec_types.end()) {
            if (err_out) *err_out = "server did not offer Apple RSA1/SRP security type 33";
            return false;
        }
        UniquePkey pub = Rsa1InitFetchKey(tcp_fd_);
        if (!pub) {
            if (err_out) *err_out = "RSA1 server public key exchange failed";
            return false;
        }

        if (use_srp) {
            std::vector<uint8_t> pkt1;
            if (!BuildSrpPacket1(pub.get(), opt_.username, &pkt1) || !SendAll(tcp_fd_, pkt1)) {
                if (err_out) *err_out = "SRP packet-1 send failed";
                return false;
            }
            uint8_t len_b[4];
            if (!RecvExact(tcp_fd_, len_b, 4)) {
                if (err_out) *err_out = "SRP challenge length read failed";
                return false;
            }
            const uint32_t s2c1_len = ReadBe32(len_b);
            if (s2c1_len < 1000 || s2c1_len > 65536) {
                if (err_out) *err_out = "SRP challenge rejected or server fell back to non-SRP";
                return false;
            }
            std::vector<uint8_t> s2c1(s2c1_len);
            SrpChallenge ch{};
            SrpClientProof proof{};
            if (!RecvExact(tcp_fd_, s2c1.data(), s2c1_len) || !ParseSrpChallenge(s2c1.data(), s2c1.size(), &ch) ||
                !SolveSrp(ch, opt_.password, &proof)) {
                if (err_out) *err_out = "SRP challenge parse/solve failed";
                return false;
            }
            if (!SendAll(tcp_fd_, BuildSrpPacket2(ch, proof))) {
                if (err_out) *err_out = "SRP packet-2 send failed";
                return false;
            }
            uint8_t m2_hdr[4], res_b[4];
            if (!RecvExact(tcp_fd_, m2_hdr, 4)) {
                if (err_out) *err_out = "SRP server proof read failed";
                return false;
            }
            const uint32_t m2_len = ReadBe32(m2_hdr);
            if (m2_len > 4096) return false;
            std::vector<uint8_t> m2_buf(m2_len);
            if ((m2_len > 0 && !RecvExact(tcp_fd_, m2_buf.data(), m2_len)) || !RecvExact(tcp_fd_, res_b, 4)) {
                if (err_out) *err_out = "SRP SecurityResult read failed";
                return false;
            }
            if (ReadBe32(res_b) != 0) {
                if (err_out) *err_out = "SRP authentication rejected (bad username or password)";
                return false;
            }
            *wrap_key = proof.wrap_key;
            auth_used_ = "srp";
            return true;
        }

        if (!RandomBytes(wrap_key->data(), 16)) return false;
        std::vector<uint8_t> pkt;
        if (!BuildNonSrpPacket(pub.get(), opt_.username, opt_.password, wrap_key->data(), &pkt) ||
            !SendAll(tcp_fd_, pkt)) {
            if (err_out) *err_out = "non-SRP credential send failed";
            return false;
        }
        uint8_t reply[8];
        if (!RecvExact(tcp_fd_, reply, 8) || ReadBe32(reply + 4) != 0) {
            if (err_out) *err_out = "non-SRP authentication rejected (bad username or password)";
            return false;
        }
        auth_used_ = "nonsrp";
        return true;
    }

    bool RunAuthWithFallback(const SessionOptions &opt, std::array<uint8_t, 16> *wrap_key, std::string *err_out) {
        if (opt.auth_mode == AuthMode::kSrp) return RunSingleAuth(true, wrap_key, err_out);
        if (opt.auth_mode == AuthMode::kNonSrp) return RunSingleAuth(false, wrap_key, err_out);
        std::string srp_err, nonsrp_err;
        if (RunSingleAuth(true, wrap_key, &srp_err)) return true;
        if (tcp_fd_ >= 0) close(tcp_fd_), tcp_fd_ = -1;
        if (RunSingleAuth(false, wrap_key, &nonsrp_err)) return true;
        if (err_out) *err_out = srp_err + "; fallback " + nonsrp_err;
        return false;
    }

    bool HandleOptionalSessionSelect(const SessionOptions &opt, const uint8_t ecb_key[16], std::string *err_out) {
        pollfd pfd{tcp_fd_, POLLIN, 0};
        if (poll(&pfd, 1, 150) <= 0) return true;
        uint8_t peek[4]{};
        if (recv(tcp_fd_, peek, 4, MSG_PEEK) != 4) return true;
        static constexpr uint8_t kSsMagic[4] = {0x00, 0x4a, 0x00, 0x01};
        if (std::memcmp(peek, kSsMagic, 4) != 0) return true;

        uint8_t ssi[76];
        if (!RecvExact(tcp_fd_, ssi, 76)) {
            if (err_out) *err_out = "SessionSelect prompt read failed";
            return false;
        }
        // Auto-login into fresh virtual session via cmd=2 with AES-128-ECB credentials.
        std::vector<uint8_t> body(200, 0);
        WriteBe16(&body[0], 2);
        body[6] = 2;
        uint8_t u64[64]{}, p64[64]{};
        std::memcpy(u64, opt.username.data(), std::min<size_t>(63, opt.username.size()));
        std::memcpy(p64, opt.password.data(), std::min<size_t>(63, opt.password.size()));
        Aes128EcbCrypt(true, ecb_key, u64, 64, &body[0x48]);
        Aes128EcbCrypt(true, ecb_key, p64, 64, &body[0x88]);
        OPENSSL_cleanse(p64, sizeof p64);

        std::vector<uint8_t> msg;
        AppendBe16(msg, 200);
        msg.insert(msg.end(), body.begin(), body.end());
        if (!SendAll(tcp_fd_, msg)) return false;

        pollfd apfd{tcp_fd_, POLLIN, 0};
        if (poll(&apfd, 1, 15000) <= 0) {
            if (err_out) *err_out = "SessionSelect ack timed out";
            return false;
        }
        uint8_t ack[4096];
        return recv(tcp_fd_, ack, sizeof ack, 0) > 0;
    }

    bool SkipCleartextRectPayload(int32_t enc, uint16_t w, uint16_t h) {
        if (enc == 1103) return false;
        if (enc == 0) {  // Raw
            const size_t bytes = size_t(w) * size_t(h) * 4;
            std::vector<uint8_t> tmp(std::min<size_t>(bytes, 65536));
            size_t left = bytes;
            while (left > 0) {
                const size_t chunk = std::min(left, tmp.size());
                if (!RecvExact(tcp_fd_, tmp.data(), chunk)) return false;
                left -= chunk;
            }
            return true;
        }
        if (enc == 6 || enc == 16) {  // zlib / ZRLE u32 length
            uint8_t lb[4];
            if (!RecvExact(tcp_fd_, lb, 4)) return false;
            const uint32_t n = ReadBe32(lb);
            std::vector<uint8_t> tmp(n);
            return n == 0 || RecvExact(tcp_fd_, tmp.data(), n);
        }
        if (enc == 1105) {  // 0x451 AppleDisplayLayout: u16 payload_len
            uint8_t lb[2];
            if (!RecvExact(tcp_fd_, lb, 2)) return false;
            const uint16_t n = ReadBe16(lb);
            std::vector<uint8_t> pay(n);
            if (n > 0 && !RecvExact(tcp_fd_, pay.data(), n)) return false;
            ParseDisplayLayout0x451(pay.data(), pay.size(), &layout_);
            return true;
        }
        if (enc == 1107) {  // 0x453 VendorKeysym: fixed 22 bytes
            uint8_t b[22];
            return RecvExact(tcp_fd_, b, 22);
        }
        if (enc == 1109 || enc == 1010 || enc == 1011 || enc == 1002) {
            // Length-prefixed Apple metadata: always carries a u16_be byte count.
            uint8_t lb[2];
            if (!RecvExact(tcp_fd_, lb, 2)) return false;
            const uint16_t n = ReadBe16(lb);
            std::vector<uint8_t> b(n);
            return n == 0 || RecvExact(tcp_fd_, b.data(), n);
        }
        if (enc == 1110) {  // 0x456 DeviceInfo: u16 msg_size (includes after u16)
            uint8_t lb[2];
            if (!RecvExact(tcp_fd_, lb, 2)) return false;
            const uint16_t n = ReadBe16(lb);
            std::vector<uint8_t> b(n);
            return n == 0 || RecvExact(tcp_fd_, b.data(), n);
        }
        return true;
    }

    bool ReadUntilEnc1103(const uint8_t wrap_key16[16], std::string *err_out) {
        const int64_t deadline = NowNs() + 10'000'000'000LL;
        while (NowNs() < deadline) {
            uint8_t mtype = 0;
            if (!RecvExact(tcp_fd_, &mtype, 1, 5000)) break;
            if (mtype == 0x00) {  // FramebufferUpdate
                uint8_t hdr[3];
                if (!RecvExact(tcp_fd_, hdr, 3)) break;
                const uint16_t nrects = ReadBe16(hdr + 1);
                for (uint16_t i = 0; i < nrects; ++i) {
                    uint8_t rh[12];
                    if (!RecvExact(tcp_fd_, rh, 12)) {
                        if (err_out) *err_out = "truncated FramebufferUpdate rect before 0x44f";
                        return false;
                    }
                    const uint16_t w = ReadBe16(rh + 4);
                    const uint16_t h = ReadBe16(rh + 6);
                    const int32_t enc = int32_t(ReadBe32(rh + 8));
                    if (enc == 1103) {  // 0x44f EncodeEncryptionInfo
                        uint8_t rekey[36];
                        if (!RecvExact(tcp_fd_, rekey, 36) || !cipher_.InitFromRekey0x44f(rekey, wrap_key16)) {
                            if (err_out) *err_out = "0x44f rekey read/unwrap failed";
                            return false;
                        }
                        return true;
                    }
                    if (!SkipCleartextRectPayload(enc, w, h)) {
                        if (err_out) *err_out = "unsupported cleartext rect before 0x44f: " + std::to_string(enc);
                        return false;
                    }
                }
            } else if (mtype == 0x02) {
                continue;  // Bell
            } else if (mtype == 0x03) {
                uint8_t hdr[7];
                if (!RecvExact(tcp_fd_, hdr, 7)) break;
                const uint32_t len = ReadBe32(hdr + 3);
                std::vector<uint8_t> cut(len);
                if (len > 0 && !RecvExact(tcp_fd_, cut.data(), len)) break;
            } else if (mtype == 0x14) {
                uint8_t b[7];
                if (!RecvExact(tcp_fd_, b, 7)) break;
            } else {
                if (err_out) *err_out = "unexpected cleartext msg before 0x44f: " + std::to_string(mtype);
                return false;
            }
        }
        if (err_out) *err_out = "timed out waiting for 0x44f enc1103 rekey";
        return false;
    }

    bool SendEncrypted(const std::vector<uint8_t> &msg) {
        std::vector<uint8_t> wire;
        return cipher_.EncryptMessage(msg, &wire) && SendAll(tcp_fd_, wire);
    }

    void InspectControlMessage(const std::vector<uint8_t> &msg) {
        if (msg.size() >= 18 && msg[0] == 0x00) {
            const uint16_t nrects = ReadBe16(&msg[2]);
            if (nrects >= 1) {
                const int32_t enc = int32_t(ReadBe32(&msg[12]));
                if (enc == 1105 && msg.size() >= 18) {
                    const uint16_t plen = ReadBe16(&msg[16]);
                    if (size_t(18) + plen <= msg.size()) {
                        ParseDisplayLayout0x451(&msg[18], plen, &layout_);
                    }
                }
            }
        }
    }

    bool ReadOneEncryptedRecord(int timeout_ms, std::vector<uint8_t> *msg_out) {
        pollfd pfd{tcp_fd_, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) return false;
        uint8_t lb[2];
        if (!RecvExact(tcp_fd_, lb, 2, 3000)) return false;
        const uint16_t clen = ReadBe16(lb);
        if (clen < 32 || (clen % 16) != 0) return false;
        std::vector<uint8_t> cbuf(clen);
        if (!RecvExact(tcp_fd_, cbuf.data(), clen, 5000)) return false;
        if (!cipher_.DecryptRecordBody(cbuf.data(), cbuf.size(), msg_out)) return false;
        InspectControlMessage(*msg_out);
        return true;
    }

    void DrainEncryptedControl(int total_ms) {
        const int64_t end = NowNs() + int64_t(total_ms) * 1'000'000LL;
        while (NowNs() < end) {
            std::vector<uint8_t> msg;
            if (!ReadOneEncryptedRecord(30, &msg)) break;
        }
    }

    bool WaitVideoAnswer(const std::vector<uint8_t> &msg_1c, std::string *err_out) {
        auto try_read_window = [&](int window_ms) -> bool {
            const int64_t deadline = NowNs() + int64_t(window_ms) * 1'000'000LL;
            while (NowNs() < deadline) {
                std::vector<uint8_t> msg;
                if (!ReadOneEncryptedRecord(100, &msg)) continue;
                if (ExtractCanvasDims(msg, &canvas_)) return true;
            }
            return false;
        };
        if (try_read_window(opt_.virtual_display ? 2500 : 1500)) return true;
        for (int attempt = 0; attempt < 16; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!SendEncrypted(msg_1c)) break;
            if (try_read_window(500)) return true;
        }
        if (err_out) *err_out = "0x1c MediaStreamOptions answer did not yield a non-zero video canvas";
        return false;
    }

    bool DrainNonBlockingTcp(std::string *err_out) {
        for (;;) {
            uint8_t chunk[4096];
            const ssize_t n = recv(tcp_fd_, chunk, sizeof chunk, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                if (err_out) *err_out = "TCP control recv error";
                return false;
            }
            if (n == 0) {
                if (err_out) *err_out = "server closed TCP session";
                return false;
            }
            tcp_rx_buf_.insert(tcp_rx_buf_.end(), chunk, chunk + n);
        }
        while (tcp_rx_buf_.size() >= 2) {
            const uint16_t clen = ReadBe16(tcp_rx_buf_.data());
            if (clen < 32 || (clen % 16) != 0) {
                if (err_out) *err_out = "malformed enc1103 record length";
                return false;
            }
            if (tcp_rx_buf_.size() < size_t(2) + clen) break;
            std::vector<uint8_t> msg;
            if (!cipher_.DecryptRecordBody(tcp_rx_buf_.data() + 2, clen, &msg)) {
                if (err_out) *err_out = "enc1103 record integrity failure";
                return false;
            }
            tcp_rx_buf_.erase(tcp_rx_buf_.begin(), tcp_rx_buf_.begin() + 2 + clen);
            InspectControlMessage(msg);
        }
        return true;
    }

    void SendVideoSrtcp(const std::vector<uint8_t> &rtcp) {
        if (rtcp.empty() || udp_video_fd_ < 0) return;
        std::vector<uint8_t> srtcp;
        if (video_tx_srtp_.EncryptRtcp(rtcp.data(), rtcp.size(), &srtcp)) {
            sendto(udp_video_fd_, srtcp.data(), srtcp.size(), 0,
                   reinterpret_cast<const sockaddr *>(&dest_video_sin_), sizeof dest_video_sin_);
        }
    }

    void SendAudioSrtcp(const std::vector<uint8_t> &rtcp) {
        if (rtcp.empty() || udp_ctrl_fd_ < 0) return;
        std::vector<uint8_t> srtcp;
        if (audio_tx_srtp_.EncryptRtcp(rtcp.data(), rtcp.size(), &srtcp)) {
            sendto(udp_ctrl_fd_, srtcp.data(), srtcp.size(), 0,
                   reinterpret_cast<const sockaddr *>(&dest_ctrl_sin_), sizeof dest_ctrl_sin_);
        }
    }

    void SendRtcpKeepalive(bool include_sr) {
        auto send_pair = [&](bool video) {
            const uint32_t ssrc = video ? offers_.video_ssrc : offers_.audio_ssrc;
            std::vector<uint8_t> pkt = BuildEmptyRr(ssrc);
            if (include_sr) {
                const auto sr = BuildEmptySr(ssrc);
                std::vector<uint8_t> compound = sr;
                compound.insert(compound.end(), pkt.begin(), pkt.end());
                pkt = std::move(compound);
            }
            if (video) SendVideoSrtcp(pkt);
            else SendAudioSrtcp(pkt);
        };
        send_pair(true);
        send_pair(false);
    }

    SessionOptions opt_{};
    in_addr dest_addr_{};
    std::string dest_ip_;
    sockaddr_in dest_ctrl_sin_{};
    sockaddr_in dest_video_sin_{};
    int tcp_fd_ = -1;
    int udp_ctrl_fd_ = -1;
    int udp_video_fd_ = -1;
    uint16_t server_w_ = 0;
    uint16_t server_h_ = 0;
    std::string auth_used_;
    StreamCipher cipher_;
    BuiltOffers offers_{};
    NegotiationKeys keys_{};
    SrtpContext video_rx_srtp_;
    SrtpContext video_tx_srtp_;
    SrtpContext audio_rx_srtp_;
    SrtpContext audio_tx_srtp_;
    NegotiatedCanvas canvas_{};
    DisplayLayout0x451 layout_{};
    H264RtpAssembler assembler_;
    EvdevKeyMapper key_mapper_;
    uint8_t buttons_mask_ = 0;
    uint16_t last_ptr_x_ = 0;
    uint16_t last_ptr_y_ = 0;
    double scroll_accum_y_ = 0.0;
    std::vector<uint8_t> tcp_rx_buf_;
    uint32_t last_media_ssrc_ = 0;
    uint64_t video_rtp_pkts_ = 0;
    uint64_t video_rtp_bytes_ = 0;
    uint64_t audio_rtp_pkts_ = 0;
    int64_t last_tx_pulse_ns_ = 0;
    uint64_t pulse_tick_ = 0;
};

}  // namespace ft_apple_hp
