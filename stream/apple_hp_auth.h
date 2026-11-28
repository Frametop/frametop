// Apple High Performance Screen Sharing client (MIT), part of apple_hp.h.
// RSA1 sign-in: SRP-6a (RFC 5054 4096-bit group, SHA-512, PBKDF2) and the non-SRP RSA + AES path.
#pragma once

#include "apple_hp_base.h"

namespace ft_apple_hp {

// ── SRP-6a (RFC 5054 4096-bit + SHA-512 + PBKDF2) & Non-SRP RSA1+AES ─────────

// RFC 3526 / RFC 5054 4096-bit MODP safe prime (hex), the group macOS uses.
constexpr const char *kRfc5054Prime4096Hex =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A92108011A723C12A787E6D7"
    "88719A10BDBA5B2699C327186AF4E23C1A946834B6150BDA2583E9CA2AD44CE8"
    "DBBBC2DB04DE8EF92E8EFC141FBECAA6287C59474E6BC05D99B2964FA090C3A2"
    "233BA186515BE7ED1F612970CEE2D7AFB81BDD762170481CD0069127D5B05AA9"
    "93B4EA988D8FDDC186FFB7DC90A6C08F4DF435C934063199FFFFFFFFFFFFFFFF";

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
    BIGNUM *raw_group = nullptr;
    if (BN_hex2bn(&raw_group, kRfc5054Prime4096Hex) <= 0) return false;
    UniqueBn bn_group(raw_group);
    if (BN_cmp(bn_N.get(), bn_group.get()) != 0 || !BN_is_word(bn_g.get(), 5)) return false;

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

}  // namespace ft_apple_hp
