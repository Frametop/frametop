// Apple High Performance Screen Sharing client (MIT), part of apple_hp.h.
// Byte order, clocks, session liveness, and the OpenSSL / zlib primitives every other part uses.
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

inline int64_t BootNs() {
#ifdef CLOCK_BOOTTIME
    timespec ts{};
    if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
        return int64_t(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }
#endif
    return NowNs();
}

// Tracks peer liveness across both normal streaming and deep-suspend wakeups.
// On Linux, CLOCK_MONOTONIC freezes during suspend while CLOCK_BOOTTIME keeps advancing:
// when (delta_boot - delta_mono) exceeds suspend_gap_ns, the client woke from suspend,
// ignores any stale pre-sleep buffered packets on that wake tick, probes the host, and
// requires fresh post-wake traffic within wake_grace_ns. During normal operation,
// sustained silence triggers a probe at rx_probe_ns and terminates the session at
// rx_timeout_ns so the supervisor (ft-screens) reconnects automatically.
class SessionLiveness {
public:
    enum class Action { kOk, kProbeWake, kProbeIdle, kTimedOutWake, kTimedOutIdle };

    void Reset(int64_t mono_ns, int64_t boot_ns,
               int64_t suspend_gap_ms = 3000, int64_t wake_grace_ms = 4000,
               int64_t rx_probe_ms = 6000, int64_t rx_timeout_ms = 15000) {
        last_mono_ns_ = mono_ns;
        last_boot_ns_ = boot_ns;
        last_rx_boot_ns_ = boot_ns;
        wake_probe_boot_ns_ = 0;
        idle_probed_ = false;
        suspend_gap_ns_ = suspend_gap_ms > 0 ? suspend_gap_ms * 1'000'000LL : 0;
        wake_grace_ns_ = wake_grace_ms > 0 ? wake_grace_ms * 1'000'000LL : 0;
        rx_probe_ns_ = rx_probe_ms > 0 ? rx_probe_ms * 1'000'000LL : 0;
        rx_timeout_ns_ = rx_timeout_ms > 0 ? rx_timeout_ms * 1'000'000LL : 0;
    }

    bool wake_probe_pending() const { return wake_probe_boot_ns_ > 0; }

    Action Step(int64_t mono_ns, int64_t boot_ns, bool got_rx) {
        const int64_t d_mono = std::max<int64_t>(0, mono_ns - last_mono_ns_);
        const int64_t d_boot = std::max<int64_t>(0, boot_ns - last_boot_ns_);
        last_mono_ns_ = mono_ns;
        last_boot_ns_ = boot_ns;

        if (suspend_gap_ns_ > 0 && d_boot - d_mono >= suspend_gap_ns_) {
            wake_probe_boot_ns_ = boot_ns;
            idle_probed_ = false;
            return Action::kProbeWake;
        }

        if (got_rx) {
            last_rx_boot_ns_ = boot_ns;
            wake_probe_boot_ns_ = 0;
            idle_probed_ = false;
            return Action::kOk;
        }

        if (wake_probe_boot_ns_ > 0) {
            if (wake_grace_ns_ > 0 && boot_ns - wake_probe_boot_ns_ >= wake_grace_ns_) {
                return Action::kTimedOutWake;
            }
            return Action::kOk;
        }

        const int64_t silence = boot_ns - last_rx_boot_ns_;
        if (rx_timeout_ns_ > 0 && silence >= rx_timeout_ns_) {
            return Action::kTimedOutIdle;
        }
        if (rx_probe_ns_ > 0 && !idle_probed_ && silence >= rx_probe_ns_) {
            idle_probed_ = true;
            return Action::kProbeIdle;
        }
        return Action::kOk;
    }

private:
    int64_t last_mono_ns_ = 0;
    int64_t last_boot_ns_ = 0;
    int64_t last_rx_boot_ns_ = 0;
    int64_t wake_probe_boot_ns_ = 0;
    bool idle_probed_ = false;
    int64_t suspend_gap_ns_ = 0;
    int64_t wake_grace_ns_ = 0;
    int64_t rx_probe_ns_ = 0;
    int64_t rx_timeout_ns_ = 0;
};

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

}  // namespace ft_apple_hp
