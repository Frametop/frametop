// Offline unit and loopback integration tests for stream/apple_hp.h.
#include "apple_hp.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

namespace {

using namespace ft_apple_hp;

int g_checks = 0;

#define CHECK(cond)                                                                             \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            std::exit(1);                                                                       \
        }                                                                                       \
        ++g_checks;                                                                             \
    } while (0)

bool RsaPkcs1Decrypt(EVP_PKEY *priv, const uint8_t *cipher, size_t cipher_len, std::vector<uint8_t> *out) {
    UniquePkeyCtx ctx(EVP_PKEY_CTX_new(priv, nullptr));
    if (!ctx || EVP_PKEY_decrypt_init(ctx.get()) <= 0) return false;
    if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING) <= 0) return false;
    size_t out_len = 0;
    if (EVP_PKEY_decrypt(ctx.get(), nullptr, &out_len, cipher, cipher_len) <= 0) return false;
    out->resize(out_len);
    if (EVP_PKEY_decrypt(ctx.get(), out->data(), &out_len, cipher, cipher_len) <= 0) return false;
    out->resize(out_len);
    return true;
}

std::vector<uint8_t> EncodeDerSpki(EVP_PKEY *pkey) {
    unsigned char *der = nullptr;
    const int len = i2d_PUBKEY(pkey, &der);
    CHECK(len > 0 && der != nullptr);
    std::vector<uint8_t> out(der, der + len);
    OPENSSL_free(der);
    return out;
}

std::vector<uint8_t> BuildSyntheticHighProfileSps(uint8_t level_idc, uint32_t max_refs) {
    BitWriter bw;
    bw.WriteBits(100, 8);  // High profile
    bw.WriteBits(0x00, 8);
    bw.WriteBits(level_idc, 8);
    bw.WriteUe(0);  // sps_id
    bw.WriteUe(1);  // chroma_format_idc = 1 (4:2:0)
    bw.WriteUe(0);  // bit_depth_luma_minus8
    bw.WriteUe(0);  // bit_depth_chroma_minus8
    bw.WriteBit(0); // qpprime_y_zero_transform_bypass_flag
    bw.WriteBit(0); // seq_scaling_matrix_present_flag
    bw.WriteUe(9);  // log2_max_frame_num_minus4 (Apple 13-bit)
    bw.WriteUe(0);  // pic_order_cnt_type = 0
    bw.WriteUe(9);  // log2_max_pic_order_cnt_lsb_minus4
    bw.WriteUe(max_refs);
    bw.WriteBit(0);    // gaps_in_frame_num_value_allowed_flag
    bw.WriteUe(239);   // (239 + 1) * 16 = 3840
    bw.WriteUe(134);   // (134 + 1) * 16 = 2160
    bw.WriteBit(1);    // frame_mbs_only_flag = 1
    bw.WriteBit(1);    // direct_8x8_inference_flag
    bw.WriteBit(0);    // frame_cropping_flag = 0
    bw.WriteBit(1);    // vui_parameters_present_flag = 1
    // Add 8 zero VUI flags (aspect..pic_struct) + bitstream_restriction_flag = 0 to exercise VUI parsing!
    bw.WriteBits(0, 9);
    bw.WriteBit(1);  // RBSP stop bit
    while ((bw.bytes().size() * 8) == 0) break;
    // Pad trailing bits to byte boundary:
    std::vector<uint8_t> rbsp = bw.bytes();
    const auto ebsp = AddEmulationPrevention(rbsp);
    std::vector<uint8_t> nal = {0x67};
    nal.insert(nal.end(), ebsp.begin(), ebsp.end());
    return nal;
}

std::vector<uint8_t> BuildAvcConfig0x92Packet(const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps) {
    std::vector<uint8_t> pkt = {0x92, 0x00, 0x00, 0x00, 'a', 'v', 'c', '1', 0x00, 0x00, 'a', 'v', 'c', 'C'};
    // AVCDecoderConfigurationRecord
    pkt.push_back(1);                                    // configurationVersion
    pkt.push_back(sps.size() > 1 ? sps[1] : 100);        // AVCProfileIndication
    pkt.push_back(sps.size() > 2 ? sps[2] : 0);          // profile_compatibility
    pkt.push_back(sps.size() > 3 ? sps[3] : 52);         // AVCLevelIndication
    pkt.push_back(0xff);                                 // lengthSizeMinusOne = 3
    pkt.push_back(0xe1);                                 // numOfSequenceParameterSets = 1
    AppendBe16(pkt, uint16_t(sps.size()));
    pkt.insert(pkt.end(), sps.begin(), sps.end());
    pkt.push_back(1);                                    // numOfPictureParameterSets = 1
    AppendBe16(pkt, uint16_t(pps.size()));
    pkt.insert(pkt.end(), pps.begin(), pps.end());
    return pkt;
}

std::vector<uint8_t> BuildMock0x1cAnswer(uint32_t cw, uint32_t ch, uint32_t tiles) {
    std::vector<uint8_t> vcfg;
    AppendPbVarint(vcfg, 1, 0x11223344u);
    AppendPbVarint(vcfg, 4, cw);
    AppendPbVarint(vcfg, 5, ch);
    AppendPbVarint(vcfg, 6, tiles);
    AppendPbVarint(vcfg, 7, 0);

    std::vector<uint8_t> top;
    AppendPbVarint(top, 1, 1);
    AppendPbBytes(top, 5, vcfg);

    std::vector<uint8_t> z;
    CHECK(ZlibCompress(top, &z));
    uint8_t u[16];
    MakeUuidV4(u);
    const auto bpl = BuildOfferBplist(7, z, FormatUuidUpper(u));

    std::vector<uint8_t> ans = {0x00, 0x00, 0x00, 0x01};
    ans.insert(ans.end(), bpl.begin(), bpl.end());
    return ans;
}

void TestBplistAndNegotiatorOffers() {
    OfferConfig cfg;
    cfg.codec = VideoCodec::kAvc;
    cfg.tiles_per_frame = 1;
    cfg.ltrp = false;
    cfg.audio_enabled = false;

    BuiltOffers offers{};
    CHECK(CreateNegotiatorOffers(cfg, &offers));
    CHECK(!offers.video_bplist.empty());
    CHECK(!offers.audio_bplist.empty());

    uint32_t vssrc = 0, assrc = 0;
    CHECK(ExtractOfferSsrc(offers.video_bplist, true, &vssrc));
    CHECK(ExtractOfferSsrc(offers.audio_bplist, false, &assrc));
    CHECK(vssrc == offers.video_ssrc);
    CHECK(assrc == offers.audio_ssrc);

    NegotiationKeys keys{};
    CHECK(NegotiationKeys::Generate(&keys));
    const auto m1c = Build0x1cMessage(offers.audio_bplist, offers.video_bplist, keys, false);
    CHECK(m1c[0] == 0x1c);
    CHECK(size_t(ReadBe16(&m1c[2])) + 4 == m1c.size());
    CHECK(ReadBe32(&m1c[6]) == 7u);  // strip cursor

    const auto ans = BuildMock0x1cAnswer(3840, 2160, 1);
    NegotiatedCanvas canvas{};
    CHECK(ExtractCanvasDims(ans, &canvas));
    CHECK(canvas.width == 3840 && canvas.height == 2160 && canvas.tiles == 1);

    // Verify macOS 26/27+ raw-zlib RFBMediaStreamMessage2 layout ([20B hdr][audio zlib][video1 zlib]).
    std::vector<uint8_t> acfg, atop, az;
    AppendPbVarint(acfg, 1, 0x55667788u);
    AppendPbVarint(atop, 1, 1);
    AppendPbBytes(atop, 3, acfg);
    CHECK(ZlibCompress(atop, &az));

    std::vector<uint8_t> vcfg, vtop, vz;
    AppendPbVarint(vcfg, 1, 0x11223344u);
    AppendPbVarint(vcfg, 4, 1920);
    AppendPbVarint(vcfg, 5, 1080);
    CHECK(ZlibCompress([&] {
        AppendPbVarint(vtop, 1, 1);
        AppendPbBytes(vtop, 5, vcfg);
        return vtop;
    }(), &vz));

    std::vector<uint8_t> raw_ans = {0x00, 0x00, 0x00, 0x01};
    raw_ans.resize(4 + 12 + 20, 0x00);
    raw_ans.insert(raw_ans.end(), az.begin(), az.end());
    raw_ans.insert(raw_ans.end(), vz.begin(), vz.end());
    NegotiatedCanvas raw_canvas{};
    CHECK(ExtractCanvasDims(raw_ans, &raw_canvas));
    CHECK(raw_canvas.width == 1920 && raw_canvas.height == 1080 && raw_canvas.tiles == 1);
}

void TestSrpAndNonSrpAuth() {
    UniquePkey rsa(EVP_RSA_gen(2048));
    CHECK(rsa != nullptr);

    // 1. Test non-SRP RSA1+AES packet construction & server decrypt.
    uint8_t aes_key[16];
    CHECK(RandomBytes(aes_key, 16));
    std::vector<uint8_t> nonsrp;
    CHECK(BuildNonSrpPacket(rsa.get(), "alice", "s3cr3t-pass!", aes_key, &nonsrp));
    CHECK(nonsrp.size() == 4 + 8 + 128 + 2 + 256);
    std::vector<uint8_t> dec_key;
    CHECK(RsaPkcs1Decrypt(rsa.get(), nonsrp.data() + 4 + 8 + 128 + 2, 256, &dec_key));
    CHECK(dec_key.size() == 16 && std::memcmp(dec_key.data(), aes_key, 16) == 0);
    uint8_t plain_creds[128];
    CHECK(Aes128EcbCrypt(false, dec_key.data(), nonsrp.data() + 4 + 8, 128, plain_creds));
    CHECK(std::strcmp(reinterpret_cast<const char *>(plain_creds), "alice") == 0);
    CHECK(std::strcmp(reinterpret_cast<const char *>(plain_creds + 64), "s3cr3t-pass!") == 0);

    // 2. Test SRP-6a 4096-bit challenge/proof agreement against server math.
    CHECK(std::strlen(kRfc5054Prime4096Hex) == kSrpModulusLen * 2);
    BIGNUM *raw_N = nullptr;
    CHECK(BN_hex2bn(&raw_N, kRfc5054Prime4096Hex) > 0);
    UniqueBn bn_N(raw_N);
    CHECK(BN_num_bits(bn_N.get()) == int(kSrpModulusLen * 8));
    UniqueBn bn_g(BN_new());
    BN_set_word(bn_g.get(), 5);
    UniqueBnCtx ctx(BN_CTX_new());

    std::vector<uint8_t> salt(32, 0x42);
    const uint64_t iters = 1000;
    const std::string pw = "correct-horse-battery-staple";

    UniqueBn bn_x_raw = DeriveSrpX(salt, iters, pw);
    UniqueBn bn_x(BN_new()), bn_v(BN_new()), bn_b(BN_new()), bn_gb(BN_new());
    BN_mod(bn_x.get(), bn_x_raw.get(), bn_N.get(), ctx.get());
    BN_mod_exp(bn_v.get(), bn_g.get(), bn_x.get(), bn_N.get(), ctx.get());
    BN_set_word(bn_b.get(), 123456789u);
    BN_mod_exp(bn_gb.get(), bn_g.get(), bn_b.get(), bn_N.get(), ctx.get());

    std::array<uint8_t, kSrpModulusLen> N_bytes{}, g_pad{}, B_pad{};
    BN_bn2binpad(bn_N.get(), N_bytes.data(), kSrpModulusLen);
    CHECK(N_bytes[0] == 0xff && N_bytes[kSrpModulusLen - 1] == 0xff);
    static constexpr uint8_t kRfc5054Prime4096Sha256[32] = {
        0x4e, 0xe9, 0x51, 0x87, 0x68, 0x2b, 0xcb, 0x23, 0x0a, 0xd2, 0x6a, 0x95, 0x20, 0x5f, 0x69, 0x20,
        0xe8, 0x47, 0x08, 0xf6, 0x25, 0x1b, 0x38, 0x94, 0x32, 0x9b, 0x09, 0xec, 0x23, 0x91, 0x9e, 0x33,
    };
    const auto n_sha256 = Sha256(N_bytes.data(), N_bytes.size());
    CHECK(std::memcmp(n_sha256.data(), kRfc5054Prime4096Sha256, 32) == 0);
    BN_bn2binpad(bn_g.get(), g_pad.data(), kSrpModulusLen);
    const auto k_hash = Sha512Parts({{N_bytes.data(), kSrpModulusLen}, {g_pad.data(), kSrpModulusLen}});
    UniqueBn bn_k(BN_bin2bn(k_hash.data(), kSrpHashLen, nullptr));
    UniqueBn bn_kv(BN_new()), bn_B(BN_new());
    BN_mod_mul(bn_kv.get(), bn_k.get(), bn_v.get(), bn_N.get(), ctx.get());
    BN_mod_add(bn_B.get(), bn_kv.get(), bn_gb.get(), bn_N.get(), ctx.get());
    BN_bn2binpad(bn_B.get(), B_pad.data(), kSrpModulusLen);

    // Build wire s2c1 challenge.
    const std::string cap_str = "mda=SHA-512,replay_detection,conf+int=ChaCha20-Poly1305,kdf=SALTED-SHA512-PBKDF2";
    std::vector<uint8_t> s2c1(12, 0);
    s2c1.push_back(0x00);
    s2c1.insert(s2c1.end(), N_bytes.begin(), N_bytes.end());
    AppendBe16(s2c1, 1);
    s2c1.push_back(5);
    s2c1.push_back(32);
    s2c1.insert(s2c1.end(), salt.begin(), salt.end());
    AppendBe16(s2c1, kSrpModulusLen);
    s2c1.insert(s2c1.end(), B_pad.begin(), B_pad.end());
    AppendBe64(s2c1, iters);
    AppendBe16(s2c1, uint16_t(cap_str.size()));
    s2c1.insert(s2c1.end(), cap_str.begin(), cap_str.end());

    SrpChallenge parsed{};
    CHECK(ParseSrpChallenge(s2c1.data(), s2c1.size(), &parsed));
    SrpClientProof proof{}, bad_proof{};
    CHECK(SolveSrp(parsed, pw, &proof));
    CHECK(SolveSrp(parsed, "wrong-password", &bad_proof));
    {
        // Only the RFC 5054 4096-bit group with g = 5: a spoofed server picking a
        // weak N, a zero-padded 3072-bit prime, or another g gets no proof.
        SrpChallenge weak = parsed;
        weak.N[kSrpModulusLen - 1] ^= 0x02;
        SrpClientProof unused{};
        CHECK(!SolveSrp(weak, pw, &unused));
        SrpChallenge n3072 = parsed;
        std::fill(n3072.N.begin(), n3072.N.begin() + 128, 0);
        CHECK(!SolveSrp(n3072, pw, &unused));
        SrpChallenge other_g = parsed;
        other_g.g = {2};
        CHECK(!SolveSrp(other_g, pw, &unused));
    }

    // Server computes S = (A * v^u)^b mod N and checks K & M1.
    UniqueBn bn_A(BN_bin2bn(proof.A.data(), kSrpModulusLen, nullptr));
    const auto u_hash = Sha512Parts({{proof.A.data(), kSrpModulusLen}, {B_pad.data(), kSrpModulusLen}});
    UniqueBn bn_u(BN_bin2bn(u_hash.data(), kSrpHashLen, nullptr));
    UniqueBn bn_vu(BN_new()), bn_Avu(BN_new()), bn_S_srv(BN_new());
    BN_mod_exp(bn_vu.get(), bn_v.get(), bn_u.get(), bn_N.get(), ctx.get());
    BN_mod_mul(bn_Avu.get(), bn_A.get(), bn_vu.get(), bn_N.get(), ctx.get());
    BN_mod_exp(bn_S_srv.get(), bn_Avu.get(), bn_b.get(), bn_N.get(), ctx.get());

    std::array<uint8_t, kSrpModulusLen> S_srv_pad{};
    BN_bn2binpad(bn_S_srv.get(), S_srv_pad.data(), kSrpModulusLen);
    const auto K_srv = Sha512Parts({{S_srv_pad.data(), kSrpModulusLen}});
    CHECK(K_srv == proof.K);
    CHECK(K_srv != bad_proof.K);
}

void TestEnc1103StreamCipher() {
    uint8_t wrap_key[16], cbc_key[16], cbc_iv[16], rekey[36]{};
    CHECK(RandomBytes(wrap_key, 16) && RandomBytes(cbc_key, 16) && RandomBytes(cbc_iv, 16));
    WriteBe32(rekey, 1);
    CHECK(Aes128EcbCrypt(true, wrap_key, cbc_key, 16, rekey + 4));
    CHECK(Aes128EcbCrypt(true, wrap_key, cbc_iv, 16, rekey + 20));

    StreamCipher cli, srv;
    CHECK(cli.InitFromRekey0x44f(rekey, wrap_key));
    CHECK(srv.InitFromRekey0x44f(rekey, wrap_key));

    for (size_t sz : {size_t(1), size_t(8), size_t(56), size_t(308), size_t(911)}) {
        const auto msg = RandomVec(sz);
        std::vector<uint8_t> wire, out;
        CHECK(cli.EncryptMessage(msg, &wire));
        CHECK(ReadBe16(wire.data()) == wire.size() - 2);
        CHECK(srv.DecryptRecordBody(wire.data() + 2, wire.size() - 2, &out));
        CHECK(out == msg);
    }

    // Tamper check on next record.
    std::vector<uint8_t> wire, out;
    CHECK(srv.EncryptMessage(std::vector<uint8_t>{0xaa, 0xbb, 0xcc}, &wire));
    wire.back() ^= 0x01;
    CHECK(!cli.DecryptRecordBody(wire.data() + 2, wire.size() - 2, &out));
}

void TestDisplayConfig0x1dAnd0x451() {
    const auto m = BuildVirtualDisplay0x1d(1920, 1080, 2.0, false);
    CHECK(m.size() == 308);
    CHECK(m[0] == 0x1d);
    CHECK(ReadBe16(&m[2]) == 304);
    CHECK(ReadBe16(&m[12]) == 296);
    CHECK(ReadBe32(&m[12 + 0x7a]) == 1);
    CHECK(ReadBe32(&m[12 + 0x7e]) == 4);
    CHECK(ReadBe32(&m[12 + 0x8a]) == 3840);
    CHECK(ReadBe32(&m[12 + 0x8e]) == 2160);
    CHECK(ReadBe32(&m[12 + 0x96]) == 7);
    CHECK(ReadBe16(&m[12 + 0x9a]) == 5);
    CHECK(ReadBe32(&m[12 + 0x9c + 0]) == 3840);
    CHECK(ReadBe32(&m[12 + 0x9c + 4]) == 2160);
    CHECK(ReadBe32(&m[12 + 0x9c + 8]) == 1920);
    CHECK(ReadBe32(&m[12 + 0x9c + 12]) == 1080);

    // 4096x1152 32:9 ultra-wide at 1x scaling raises max_width to 4096 and keeps all modes even.
    const auto uw = BuildVirtualDisplay0x1d(4096, 1152, 1.0, false);
    CHECK(uw.size() == 308);
    CHECK(ReadBe32(&uw[12 + 0x8a]) == 4096);
    CHECK(ReadBe32(&uw[12 + 0x8e]) == 2160);
    CHECK(ReadBe32(&uw[12 + 0x9c + 0]) == 4096);
    CHECK(ReadBe32(&uw[12 + 0x9c + 4]) == 1152);
    CHECK(ReadBe32(&uw[12 + 0x9c + 8]) == 4096);
    CHECK(ReadBe32(&uw[12 + 0x9c + 12]) == 1152);
    for (size_t i = 0; i < 5; ++i) {
        const size_t off = 12 + 0x9c + 28 * i;
        CHECK((ReadBe32(&uw[off + 0]) & 1u) == 0);
        CHECK((ReadBe32(&uw[off + 4]) & 1u) == 0);
        CHECK((ReadBe32(&uw[off + 8]) & 1u) == 0);
        CHECK((ReadBe32(&uw[off + 12]) & 1u) == 0);
    }

    SessionOptions opt{};
    ResolveAppleDisplayMode("apple:virtual", 3840, 2160, &opt);
    CHECK(opt.virtual_display && opt.hidpi_scale == 2.0 && opt.logical_w == 1920 && opt.logical_h == 1080);
    ResolveAppleDisplayMode("apple:virtual", 4096, 1152, &opt);
    CHECK(opt.virtual_display && opt.hidpi_scale == 1.0 && opt.logical_w == 4096 && opt.logical_h == 1152);
    ResolveAppleDisplayMode("apple:virtual@1", 4096, 1152, &opt);
    CHECK(opt.virtual_display && opt.hidpi_scale == 1.0 && opt.logical_w == 4096 && opt.logical_h == 1152);
    ResolveAppleDisplayMode("apple:virtual", 5120, 1440, &opt);
    CHECK(opt.virtual_display && opt.hidpi_scale == 1.0 && opt.logical_w == 4096 && opt.logical_h == 1152);
    ResolveAppleDisplayMode("apple:virtual@2", 2048, 900, &opt);
    CHECK(opt.virtual_display && opt.hidpi_scale == 2.0 && opt.logical_w == 2048 && opt.logical_h == 900);
    ResolveAppleDisplayMode("apple:physical", 4096, 1152, &opt);
    CHECK(!opt.virtual_display);

    std::vector<uint8_t> l451(20 + 56, 0);
    WriteBe16(&l451[0], 5);
    WriteBe16(&l451[2], 1920);
    WriteBe16(&l451[4], 1080);
    WriteBe16(&l451[6], 3840);
    WriteBe16(&l451[8], 2160);
    WriteBe16(&l451[18], 1);
    DisplayLayout0x451 parsed{};
    CHECK(ParseDisplayLayout0x451(l451.data(), l451.size(), &parsed));
    CHECK(parsed.scaled_w == 1920 && parsed.backing_w == 3840 && parsed.display_count == 1);
}

void TestSrtpAndSrtcp() {
    uint8_t blob[46];
    CHECK(RandomBytes(blob, sizeof blob));
    SrtpContext tx, rx;
    CHECK(tx.InitFromBlob(blob) && rx.InitFromBlob(blob));

    // Test RTP header with 1 CSRC + 1-word RTP extension, plus ROC rollover 65534 -> 65535 -> 0 -> 1.
    const uint16_t seqs[] = {65534, 65535, 0, 1};
    const uint32_t rocs[] = {0, 0, 1, 1};
    for (int k = 0; k < 4; ++k) {
        std::vector<uint8_t> rtp = {0x91, 0xe4};  // V=2, X=1, CC=1, M=1, PT=100
        AppendBe16(rtp, seqs[k]);
        AppendBe32(rtp, 90000u * uint32_t(k + 1));
        AppendBe32(rtp, 0xdeadbeefu);
        AppendBe32(rtp, 0x11112222u);             // 1 CSRC
        AppendBe16(rtp, 0xbede);                  // ext profile
        AppendBe16(rtp, 1);                       // 1 ext word
        AppendBe32(rtp, 0xabcdef01u);
        const std::vector<uint8_t> pay = {0x65, 0x88, uint8_t(k), 0x42, 0x99};
        rtp.insert(rtp.end(), pay.begin(), pay.end());

        std::vector<uint8_t> srtp, out_pay;
        SrtpContext::RtpHeader hdr{};
        CHECK(tx.EncryptRtp(rtp.data(), rtp.size(), rocs[k], &srtp));
        CHECK(rx.DecryptRtp(srtp.data(), srtp.size(), &hdr, &out_pay));
        CHECK(hdr.pt == 100 && hdr.marker && hdr.seq == seqs[k] && hdr.ssrc == 0xdeadbeefu);
        CHECK(hdr.pkt_index == ((uint64_t(rocs[k]) << 16) | seqs[k]));
        CHECK(out_pay == pay);

        auto tampered = srtp;
        tampered[25] ^= 0x80;
        CHECK(!rx.DecryptRtp(tampered.data(), tampered.size(), &hdr, &out_pay));
    }

    // SRTCP roundtrip.
    const auto sr = BuildEmptySr(0x12345678u);
    std::vector<uint8_t> srtcp, out_rtcp;
    CHECK(tx.EncryptRtcp(sr.data(), sr.size(), &srtcp));
    CHECK(rx.DecryptRtcp(srtcp.data(), srtcp.size(), &out_rtcp));
    CHECK(out_rtcp == sr);
}

void TestAvcConfig0x92AndSpsPatch() {
    const auto raw_sps = BuildSyntheticHighProfileSps(52, 8);
    const std::vector<uint8_t> raw_pps = {0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0};
    const auto pkt92 = BuildAvcConfig0x92Packet(raw_sps, raw_pps);

    std::vector<uint8_t> parsed_sps, parsed_pps;
    CHECK(ParseAvcConfig0x92(pkt92.data(), pkt92.size(), &parsed_sps, &parsed_pps));
    CHECK(parsed_sps == raw_sps && parsed_pps == raw_pps);

    H264SpsInfo before{}, after{};
    const auto patched = PatchH264SpsDpb(parsed_sps, &before);
    CHECK(before.profile_idc == 100 && before.level_idc == 52);
    CHECK(before.max_num_ref_frames == 8);
    CHECK(before.width == 3840 && before.height == 2160);

    CHECK(InspectH264Sps(patched, &after));
    CHECK(after.profile_idc == 100 && after.level_idc == 60);
    CHECK(after.max_num_ref_frames == 16);
    CHECK(after.has_bitstream_restriction && after.max_num_reorder_frames == 0 && after.max_dec_frame_buffering == 16);
    CHECK(after.width == 3840 && after.height == 2160);

    // Ensure no illegal 00 00 00 / 00 00 01 / 00 00 02 sequences exist inside patched NAL.
    size_t zeros = 0;
    for (size_t i = 1; i < patched.size(); ++i) {
        if (zeros >= 2) CHECK(patched[i] >= 0x03);
        zeros = (patched[i] == 0x00) ? zeros + 1 : 0;
    }

    // Visible crop resolution trims padded macroblock rows/columns (< 16 px) while
    // ignoring stale/unrelated display hints (>= 16 px or larger than decoded).
    uint32_t cw = 0, ch = 0;
    ResolveVisibleCrop(3456, 976, 3456, 972, 3456, 972, &cw, &ch);
    CHECK(cw == 3456 && ch == 972);
    ResolveVisibleCrop(3456, 976, 3456, 976, 3456, 972, &cw, &ch);
    CHECK(cw == 3456 && ch == 972);
    ResolveVisibleCrop(3024, 1968, 3024, 1964, 1512, 982, &cw, &ch);
    CHECK(cw == 3024 && ch == 1964);
    ResolveVisibleCrop(3840, 2160, 3840, 2160, 3840, 2160, &cw, &ch);
    CHECK(cw == 3840 && ch == 2160);
    ResolveVisibleCrop(3456, 976, 3456, 976, 1728, 486, &cw, &ch);
    CHECK(cw == 3456 && ch == 976);
    ResolveVisibleCrop(1920, 1088, 1920, 1088, 3840, 2160, &cw, &ch);
    CHECK(cw == 1920 && ch == 1088);
}

void TestH264RtpAssembler() {
    H264RtpAssembler asm_h264;
    const auto raw_sps = BuildSyntheticHighProfileSps(52, 8);
    const std::vector<uint8_t> raw_pps = {0x68, 0xce, 0x3c, 0x80};
    const auto pkt92 = BuildAvcConfig0x92Packet(raw_sps, raw_pps);

    std::vector<H264RtpAssembler::AccessUnit> aus;
    SrtpContext::RtpHeader h{};
    h.pt = 100;
    h.ssrc = 0x55aa1122u;
    h.seq = 10;
    h.timestamp = 9000;
    h.marker = true;
    asm_h264.PushPacket(h, pkt92, &aus);
    CHECK(aus.empty() && asm_h264.has_config());

    // Build an intra non-IDR slice (NAL type 1, first_mb=0, slice_type=7 -> keyframe!) split across 2 FU-A packets.
    BitWriter sh;
    sh.WriteUe(0);  // first_mb_in_slice = 0
    sh.WriteUe(7);  // slice_type = 7 (I-slice)
    sh.WriteUe(0);  // pps_id = 0
    sh.WriteBits(0xaa55aa55u, 32);
    std::vector<uint8_t> slice_body = sh.bytes();
    slice_body.resize(40, 0x77);

    std::vector<uint8_t> fu1 = {0x7c, uint8_t(0x80 | 1)};  // FU-A start, inner type 1
    fu1.insert(fu1.end(), slice_body.begin(), slice_body.begin() + 20);
    std::vector<uint8_t> fu2 = {0x7c, uint8_t(0x40 | 1)};  // FU-A end, inner type 1
    fu2.insert(fu2.end(), slice_body.begin() + 20, slice_body.end());

    h.seq = 11;
    h.timestamp = 10500;
    h.marker = false;
    asm_h264.PushPacket(h, fu1, &aus);
    CHECK(aus.empty());

    h.seq = 12;
    h.marker = true;
    asm_h264.PushPacket(h, fu2, &aus);
    CHECK(aus.size() == 1);
    CHECK(aus[0].is_keyframe && aus[0].config_updated);

    // Next packet skips seq 13 -> seq 14 single NAL P-slice (first_mb=0, slice_type=0).
    aus.clear();
    BitWriter psh;
    psh.WriteUe(0);
    psh.WriteUe(0);  // P-slice
    std::vector<uint8_t> single_nal = {0x41};
    single_nal.insert(single_nal.end(), psh.bytes().begin(), psh.bytes().end());
    h.seq = 14;
    h.timestamp = 12000;
    h.marker = true;
    asm_h264.PushPacket(h, single_nal, &aus);
    CHECK(aus.size() == 1 && !aus[0].is_keyframe);
    CHECK(asm_h264.lost_packets() == 1);
    uint32_t nack_ssrc = 0;
    const auto nacks = asm_h264.TakePendingNacks(&nack_ssrc);
    CHECK(nack_ssrc == 0x55aa1122u && nacks.count(13) == 1);
}

void TestFullLoopbackSession() {
    // Ephemeral TCP listener on 127.0.0.1 + distinct server/client UDP port pairs on loopback.
    const int srv_listen = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(srv_listen >= 0);
    int one = 1;
    setsockopt(srv_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    CHECK(bind(srv_listen, reinterpret_cast<const sockaddr *>(&sin), sizeof sin) == 0);
    socklen_t slen = sizeof sin;
    CHECK(getsockname(srv_listen, reinterpret_cast<sockaddr *>(&sin), &slen) == 0);
    const uint16_t tcp_port = ntohs(sin.sin_port);
    CHECK(listen(srv_listen, 4) == 0);

    // Find a free consecutive UDP port pair for the client's ctrl/video receivers.
    uint16_t cli_udp_base = 0;
    for (uint16_t p = 39200; p < 39900; p += 2) {
        const int u0 = BindUdpIpv4("127.0.0.1", p);
        const int u1 = BindUdpIpv4("127.0.0.1", uint16_t(p + 1));
        if (u0 >= 0 && u1 >= 0) {
            close(u0);
            close(u1);
            cli_udp_base = p;
            break;
        }
        if (u0 >= 0) close(u0);
        if (u1 >= 0) close(u1);
    }
    CHECK(cli_udp_base != 0);

    UniquePkey srv_rsa(EVP_RSA_gen(2048));
    const auto srv_spki = EncodeDerSpki(srv_rsa.get());

    std::atomic<bool> srv_ok{false};
    std::thread server_thread([&]() {
        const int cfd = accept(srv_listen, nullptr, nullptr);
        if (cfd < 0) return;

        // 1. RFB 003.889 + security types [30, 33, 36, 35].
        const uint8_t ver[12] = {'R', 'F', 'B', ' ', '0', '0', '3', '.', '8', '8', '9', '\n'};
        uint8_t cver[12];
        if (!SendAll(cfd, ver, 12) || !RecvExact(cfd, cver, 12)) {
            close(cfd);
            return;
        }
        const uint8_t types[5] = {4, 30, 33, 36, 35};
        uint8_t rsa_init[15];
        if (!SendAll(cfd, types, 5) || !RecvExact(cfd, rsa_init, 15)) {
            close(cfd);
            return;
        }

        // 2. Reply with RSA-2048 SubjectPublicKeyInfo.
        std::vector<uint8_t> key_pkt = {0x00, 0x00};
        AppendBe32(key_pkt, uint32_t(srv_spki.size()));
        key_pkt.insert(key_pkt.end(), srv_spki.begin(), srv_spki.end());
        std::vector<uint8_t> key_wire;
        AppendBe32(key_wire, uint32_t(key_pkt.size()));
        key_wire.insert(key_wire.end(), key_pkt.begin(), key_pkt.end());
        if (!SendAll(cfd, key_wire)) {
            close(cfd);
            return;
        }

        // 3. Read non-SRP packet (client uses AuthMode::kNonSrp in fast loopback test), decrypt AES wrap key.
        uint8_t auth_len_b[4];
        if (!RecvExact(cfd, auth_len_b, 4)) {
            close(cfd);
            return;
        }
        std::vector<uint8_t> auth_body(ReadBe32(auth_len_b));
        if (!RecvExact(cfd, auth_body.data(), auth_body.size())) {
            close(cfd);
            return;
        }
        std::vector<uint8_t> wrap_key;
        if (!RsaPkcs1Decrypt(srv_rsa.get(), auth_body.data() + 8 + 128 + 2, 256, &wrap_key) || wrap_key.size() != 16) {
            close(cfd);
            return;
        }
        const uint8_t auth_ok[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        uint8_t cinit = 0;
        if (!SendAll(cfd, auth_ok, 8) || !RecvExact(cfd, &cinit, 1)) {
            close(cfd);
            return;
        }

        // 4. Send ServerInit (3840x2160, name "Test").
        std::vector<uint8_t> sinit(24, 0);
        WriteBe16(&sinit[0], 3840);
        WriteBe16(&sinit[2], 2160);
        WriteBe32(&sinit[20], 4);
        sinit.insert(sinit.end(), {'T', 'e', 's', 't'});
        if (!SendAll(cfd, sinit)) {
            close(cfd);
            return;
        }

        // 5. Read ViewerInfo (66B) + 0x12 followup (12B) + 0x1d (308B) + SetEncodings (56B).
        std::vector<uint8_t> pre(66 + 12 + 308 + 56);
        if (!RecvExact(cfd, pre.data(), pre.size())) {
            close(cfd);
            return;
        }

        // 6. Send cleartext 0x451 layout + 0x44f (1103) rekey in one FramebufferUpdate.
        uint8_t cbc_key[16], cbc_iv[16], rekey[36]{};
        RandomBytes(cbc_key, 16);
        RandomBytes(cbc_iv, 16);
        WriteBe32(rekey, 1);
        Aes128EcbCrypt(true, wrap_key.data(), cbc_key, 16, rekey + 4);
        Aes128EcbCrypt(true, wrap_key.data(), cbc_iv, 16, rekey + 20);

        StreamCipher srv_cipher;
        srv_cipher.InitFromRekey0x44f(rekey, wrap_key.data());

        std::vector<uint8_t> fbu = {0x00, 0x00, 0x00, 0x01};  // 1 rect: 1103
        fbu.insert(fbu.end(), 8, 0);
        AppendBe32(fbu, 1103);
        fbu.insert(fbu.end(), rekey, rekey + 36);
        uint8_t post_toggle[8];
        if (!SendAll(cfd, fbu) || !RecvExact(cfd, post_toggle, 8)) {
            close(cfd);
            return;
        }

        // 7. Read encrypted client messages (0x1d, SetEncodings) until encrypted 0x1c offer arrives.
        auto read_enc = [&](std::vector<uint8_t> *m) -> bool {
            uint8_t lb[2];
            if (!RecvExact(cfd, lb, 2)) return false;
            std::vector<uint8_t> cb(ReadBe16(lb));
            return RecvExact(cfd, cb.data(), cb.size()) && srv_cipher.DecryptRecordBody(cb.data(), cb.size(), m);
        };
        std::vector<uint8_t> enc_1c;
        bool saw_1d = false, saw_set = false;
        for (int i = 0; i < 6; ++i) {
            std::vector<uint8_t> msg;
            if (!read_enc(&msg) || msg.empty()) break;
            if (msg[0] == 0x1d) saw_1d = true;
            else if (msg[0] == 0x02) saw_set = true;
            else if (msg[0] == 0x1c) {
                enc_1c = std::move(msg);
                break;
            }
        }
        if (!saw_1d || !saw_set || enc_1c.size() < 0x80) {
            close(cfd);
            return;
        }
        const uint16_t as = ReadBe16(&enc_1c[10]);
        const size_t vks_off = 0x80 + size_t(as) + 0x2e;
        uint8_t vks[46];
        std::memcpy(vks, &enc_1c[vks_off], 46);

        // 8. Send encrypted 0x1c answer (3840x2160, 1 tile).
        std::vector<uint8_t> ans_wire;
        srv_cipher.EncryptMessage(BuildMock0x1cAnswer(3840, 2160, 1), &ans_wire);
        if (!SendAll(cfd, ans_wire)) {
            close(cfd);
            return;
        }

        // 9. Transmit SRTP 0x92 avcC config + SRTP FU-A H.264 intra frame to client's UDP video port!
        SrtpContext srv_srtp;
        srv_srtp.InitFromBlob(vks);
        const int ufd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        dst.sin_port = htons(uint16_t(cli_udp_base + 1));

        auto send_srtp = [&](uint16_t seq, uint32_t ts, bool marker, const std::vector<uint8_t> &pay) {
            std::vector<uint8_t> rtp = {0x80, uint8_t((marker ? 0x80 : 0x00) | 100)};
            AppendBe16(rtp, seq);
            AppendBe32(rtp, ts);
            AppendBe32(rtp, 0xCAFEBABEu);
            rtp.insert(rtp.end(), pay.begin(), pay.end());
            std::vector<uint8_t> srtp;
            srv_srtp.EncryptRtp(rtp.data(), rtp.size(), 0, &srtp);
            sendto(ufd, srtp.data(), srtp.size(), 0, reinterpret_cast<const sockaddr *>(&dst), sizeof dst);
        };

        const auto sps = BuildSyntheticHighProfileSps(52, 8);
        const std::vector<uint8_t> pps = {0x68, 0xce, 0x3c, 0x80};
        send_srtp(1, 9000, true, BuildAvcConfig0x92Packet(sps, pps));

        BitWriter sh;
        sh.WriteUe(0);
        sh.WriteUe(7);  // intra slice
        std::vector<uint8_t> slice = sh.bytes();
        slice.resize(32, 0x5a);
        std::vector<uint8_t> fu1 = {0x7c, 0x81}, fu2 = {0x7c, 0x41};
        fu1.insert(fu1.end(), slice.begin(), slice.begin() + 16);
        fu2.insert(fu2.end(), slice.begin() + 16, slice.end());
        send_srtp(2, 12000, false, fu1);
        send_srtp(3, 12000, true, fu2);

        // 10. Read encrypted runtime input records (Right click, Middle click, scroll while held,
        //     Left Ctrl -> 0x10 Globe down, and Blur -> 0x10 Globe up + pointer release).
        bool saw_right = false, saw_right_mid = false, saw_pure_scroll = false;
        bool saw_globe_down = false, saw_globe_up = false, saw_btn_clear = false;
        for (int i = 0; i < 8; ++i) {
            std::vector<uint8_t> imsg;
            if (!read_enc(&imsg) || imsg.empty()) break;
            if (imsg[0] == 0x05 && imsg.size() == 6) {
                if (imsg[1] == 0x02) saw_right = true;
                else if (imsg[1] == 0x06) saw_right_mid = true;
                else if (imsg[1] == 0x08) saw_pure_scroll = true;
                else if (imsg[1] == 0x00 && saw_right_mid) saw_btn_clear = true;
            } else if (imsg[0] == 0x10) {
                bool g_down = false;
                uint16_t g_vk = 0, g_kbt = 0;
                if (ParseAppleKeycodeEvent0x10(srv_cipher.cbc_key().data(), imsg.data(), imsg.size(),
                                               &g_down, &g_vk, &g_kbt) &&
                    g_vk == kMacVkFunction && g_kbt == kAppleKeyboardTypeAnsi) {
                    if (g_down) saw_globe_down = true;
                    else if (saw_globe_down) saw_globe_up = true;
                }
            }
            if (saw_globe_up && saw_btn_clear) break;
        }

        close(ufd);
        close(cfd);
        srv_ok = saw_right && saw_right_mid && saw_pure_scroll && saw_globe_down && saw_globe_up && saw_btn_clear;
    });

    SessionOptions opt;
    opt.host = "127.0.0.1";
    opt.port = tcp_port;
    opt.bind_ip = "127.0.0.1";
    opt.udp_ctrl_port = cli_udp_base;
    opt.udp_video_port = uint16_t(cli_udp_base + 1);
    opt.username = "alice";
    opt.password = "pw";
    opt.auth_mode = AuthMode::kNonSrp;
    opt.warmup_tcp = false;
    opt.settle_ms = 5;
    opt.toggle_settle_ms = 5;
    ApplyKeyboardSettings("dvorak", {{"capslock", "leftctrl"}, {"leftctrl", "globe"}}, &opt.keyboard);

    SessionClient client;
    std::string err;
    CHECK(client.Connect(opt, &err));
    CHECK(client.canvas().width == 3840 && client.canvas().height == 2160 && client.canvas().tiles == 1);

    std::vector<H264RtpAssembler::AccessUnit> aus;
    for (int i = 0; i < 20 && aus.empty(); ++i) {
        client.PollStep(20, &aus);
    }
    CHECK(client.SendMouseButton(0x111, true));   // BTN_RIGHT -> 0x02
    CHECK(client.SendMouseButton(0x112, true));   // BTN_MIDDLE -> 0x02 | 0x04 = 0x06
    CHECK(client.SendMouseScroll(0.0, -2.0));     // 2 pure 0x08 scroll-up ticks even while buttons held
    CHECK(client.SendEvdevKey(29, true));         // Physical Left Ctrl -> 0x10 Globe (kVK_Function = 0x3f) down
    CHECK(client.SendInputBlur());                // Releases 0x10 Globe up + clears mouse buttons to 0x00
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    client.Close();
    server_thread.join();
    close(srv_listen);

    CHECK(srv_ok.load());
    CHECK(aus.size() == 1);
    CHECK(aus[0].is_keyframe);
    CHECK(client.assembler().raw_sps_info().max_num_ref_frames == 8);

    // Confirm the Annex-B output starts with 00 00 00 01 67 (patched SPS with max_num_ref_frames == 16).
    CHECK(aus[0].annex_b.size() > 20);
    CHECK(aus[0].annex_b[0] == 0 && aus[0].annex_b[1] == 0 && aus[0].annex_b[2] == 0 &&
          aus[0].annex_b[3] == 1 && aus[0].annex_b[4] == 0x67);
    // Find second start code to slice out patched SPS NAL and inspect it.
    size_t second_sc = 0;
    for (size_t i = 5; i + 4 <= aus[0].annex_b.size(); ++i) {
        if (aus[0].annex_b[i] == 0 && aus[0].annex_b[i + 1] == 0 &&
            aus[0].annex_b[i + 2] == 0 && aus[0].annex_b[i + 3] == 1) {
            second_sc = i;
            break;
        }
    }
    CHECK(second_sc > 5);
    const std::vector<uint8_t> out_sps(aus[0].annex_b.begin() + 4, aus[0].annex_b.begin() + ssize_t(second_sc));
    H264SpsInfo patched_info{};
    CHECK(InspectH264Sps(out_sps, &patched_info));
    CHECK(patched_info.level_idc == 60 && patched_info.max_num_ref_frames == 16);
}

void TestZeroDelayPoc2Rewrite() {
    // Build a VideoToolbox-style CABAC PPS (pps_id=0, sps_id=0, entropy_coding_mode=1,
    // bottom_field_pic_order=0, slice_groups_m1=0, l0=3, l1=0, weighted_pred=0, wbi=0,
    // init_qp=0, init_qs=0, chroma_qp=0, deblocking_ctrl=1, constrained_intra=0, redundant=0).
    BitWriter pbw;
    pbw.WriteUe(0);
    pbw.WriteUe(0);
    pbw.WriteBit(1);  // entropy_coding_mode_flag = 1 (CABAC)
    pbw.WriteBit(0);  // bottom_field_pic_order_in_frame_present_flag = 0
    pbw.WriteUe(0);   // num_slice_groups_minus1 = 0
    pbw.WriteUe(3);   // num_ref_idx_l0_default_active_minus1
    pbw.WriteUe(0);   // num_ref_idx_l1_default_active_minus1
    pbw.WriteBit(0);  // weighted_pred_flag = 0
    pbw.WriteBits(0, 2);
    pbw.WriteUe(0);   // pic_init_qp_minus26 = 0
    pbw.WriteUe(0);   // pic_init_qs_minus26 = 0
    pbw.WriteUe(0);   // chroma_qp_index_offset = 0
    pbw.WriteBit(1);  // deblocking_filter_control_present_flag = 1
    pbw.WriteBit(0);
    pbw.WriteBit(0);
    pbw.WriteBit(1);  // RBSP stop bit
    const auto pps_ebsp = AddEmulationPrevention(pbw.bytes());
    std::vector<uint8_t> pps = {0x28};
    pps.insert(pps.end(), pps_ebsp.begin(), pps_ebsp.end());

    H264PpsInfo pinfo{};
    CHECK(InspectH264Pps(pps, &pinfo));
    CHECK(pinfo.entropy_coding_mode_flag && !pinfo.weighted_pred_flag && pinfo.deblocking_filter_control_present_flag);

    const auto sps = BuildSyntheticHighProfileSps(42, 4);  // log2_max_fn_m4=9, poc_type=0, log2_max_poc_lsb_m4=9
    H264SpsInfo raw_s{};
    bool rewrote = false;
    const auto sps_poc2 = PatchH264SpsPoc2(sps, pinfo, &raw_s, &rewrote);
    CHECK(rewrote);
    H264SpsInfo chk{};
    CHECK(InspectH264Sps(sps_poc2, &chk));
    CHECK(chk.pic_order_cnt_type == 2 && chk.level_idc == 60 && chk.max_num_ref_frames == 16);

    // Build a synthetic CABAC P-slice (nal_type=1, nri=1) with 13-bit poc_lsb = 0x1555 and a CABAC
    // payload starting with 0x00 0x00 0x01 0xab 0xcd to exercise cross-boundary 00 00 03 escaping!
    BitWriter sbw;
    sbw.WriteUe(0);           // first_mb_in_slice = 0
    sbw.WriteUe(5);           // slice_type = 5 (P)
    sbw.WriteUe(0);           // pps_id = 0
    sbw.WriteBits(7, 13);     // frame_num = 7 (9 + 4 = 13 bits)
    sbw.WriteBits(0x1555, 13);// pic_order_cnt_lsb (9 + 4 = 13 bits) -> stripped!
    sbw.WriteBit(0);          // num_ref_idx_active_override_flag = 0
    sbw.WriteBit(0);          // ref_pic_list_modification_flag_l0 = 0
    sbw.WriteBit(0);          // adaptive_ref_pic_marking_mode_flag = 0
    sbw.WriteUe(0);           // cabac_init_idc = 0
    sbw.WriteUe(2);           // slice_qp_delta = -1 (se code 2)
    sbw.WriteUe(1);           // disable_deblocking_filter_idc = 1
    size_t sbw_bits = 1 + 5 + 1 + 13 + 13 + 3 + 1 + 3 + 3;
    while ((sbw_bits & 7) != 0) {
        sbw.WriteBit(1);      // cabac_alignment_one_bit
        ++sbw_bits;
    }
    const std::vector<uint8_t> cabac_payload = {0x00, 0x00, 0x01, 0xab, 0xcd, 0x00, 0x00, 0x02, 0xef};
    for (uint8_t b : cabac_payload) sbw.WriteBits(b, 8);

    const auto slice_ebsp = AddEmulationPrevention(sbw.bytes());
    std::vector<uint8_t> slice_nal = {0x21};
    slice_nal.insert(slice_nal.end(), slice_ebsp.begin(), slice_ebsp.end());

    const auto patched_slice = PatchH264SliceHeaderPoc2(slice_nal, raw_s, pinfo);
    CHECK(patched_slice != slice_nal);
    const auto patched_rbsp = RemoveEmulationPrevention(patched_slice.data() + 1, patched_slice.size() - 1);

    // Parse patched_rbsp under poc_type == 2 (no pic_order_cnt_lsb!) and verify CABAC payload matches bit-for-bit!
    BitReader vbr(patched_rbsp.data(), patched_rbsp.size());
    uint32_t fmb = 99, st = 99, pid = 99, fn = 99, f0 = 1, f1 = 1, f2 = 1, cid = 99, dis = 99;
    int32_t qpd = 0;
    CHECK(vbr.ReadUe(&fmb) && fmb == 0);
    CHECK(vbr.ReadUe(&st) && st == 5);
    CHECK(vbr.ReadUe(&pid) && pid == 0);
    CHECK(vbr.ReadBits(13, &fn) && fn == 7);
    CHECK(vbr.ReadBits(1, &f0) && f0 == 0);
    CHECK(vbr.ReadBits(1, &f1) && f1 == 0);
    CHECK(vbr.ReadBits(1, &f2) && f2 == 0);
    CHECK(vbr.ReadUe(&cid) && cid == 0);
    CHECK(vbr.ReadSe(&qpd) && qpd == -1);
    CHECK(vbr.ReadUe(&dis) && dis == 1);
    while ((vbr.pos() & 7) != 0) {
        uint32_t one = 0;
        CHECK(vbr.ReadBits(1, &one) && one == 1);
    }
    const size_t pay_off = vbr.pos() >> 3;
    CHECK(patched_rbsp.size() - pay_off == cabac_payload.size());
    CHECK(std::memcmp(patched_rbsp.data() + pay_off, cabac_payload.data(), cabac_payload.size()) == 0);
}

void TestInputKeyMappingAndMetaHoldback() {
    const auto ke = BuildKeyEvent(true, 0xffeb);
    CHECK(ke.size() == 8 && ke[0] == 0x04 && ke[1] == 1 && ReadBe32(&ke[4]) == 0xffeb);
    const auto pe = BuildPointerEvent(0x05, 960, 540);
    CHECK(pe.size() == 6 && pe[0] == 0x05 && pe[1] == 0x05 && ReadBe16(&pe[2]) == 960 && ReadBe16(&pe[4]) == 540);

    // ScreensharingAgent's hidpi_ConvertActualLocationsToVirtual expects backing ("Actual")
    // pixel coordinates (e.g. 3840x2160 on 1920x1080@2x Retina) and divides by HiDPI scale
    // on the host; when stream_dim != backing_dim (e.g. 5K backing over 4K stream), ScalePointerCoord
    // scales into backing_dim and clamps to int16_t max (32767).
    CHECK(ScalePointerCoord(0.0, 3840, 3840) == 0);
    CHECK(ScalePointerCoord(1920.0, 3840, 3840) == 1920);
    CHECK(ScalePointerCoord(3839.0, 3840, 3840) == 3839);
    CHECK(ScalePointerCoord(9999.0, 3840, 3840) == 3839);
    CHECK(ScalePointerCoord(3839.0, 3840, 5120) == 5119);
    CHECK(ScalePointerCoord(40000.0, 40000, 40000) == 32767);
    // Apple viewer CGMouseButton bit order: 0x110 (Left) -> 0x01, 0x111 (Right) -> 0x02, 0x112 (Middle) -> 0x04.
    CHECK(MouseButtonBit(0x110) == 0x01 && MouseButtonBit(0x111) == 0x02 && MouseButtonBit(0x112) == 0x04);

    // Verify Apple 0x10 EncryptedEventMessage roundtrip for kVK_Function (0x3f = Globe / Fn).
    uint8_t ecb_key[16]{};
    CHECK(RandomBytes(ecb_key, sizeof ecb_key));
    std::vector<uint8_t> pkt10;
    CHECK(BuildAppleKeycodeEvent0x10(ecb_key, true, kMacVkFunction, &pkt10));
    CHECK(pkt10.size() == 18 && pkt10[0] == 0x10 && pkt10[1] == 0x03);
    bool p_down = false;
    uint16_t p_vk = 0, p_kbt = 0;
    uint32_t p_ks = 99;
    CHECK(ParseAppleKeycodeEvent0x10(ecb_key, pkt10.data(), pkt10.size(), &p_down, &p_vk, &p_kbt, &p_ks));
    CHECK(p_down && p_vk == 0x003f && p_kbt == 0x003d && p_ks == 0);
    CHECK(BuildAppleKeycodeEvent0x10(ecb_key, false, kMacVkFunction, &pkt10));
    CHECK(ParseAppleKeycodeEvent0x10(ecb_key, pkt10.data(), pkt10.size(), &p_down, &p_vk, &p_kbt, &p_ks));
    CHECK(!p_down && p_vk == 0x003f && p_kbt == 0x003d);
    pkt10[0] = 0x04;
    CHECK(!ParseAppleKeycodeEvent0x10(ecb_key, pkt10.data(), pkt10.size(), &p_down, &p_vk));

    // Verify Command vs Option keysym mapping matches ScreensharingAgent KeyMap.c:
    // KEY_LEFTMETA (125) -> 0xffeb (Super_L -> Command), KEY_LEFTALT (56) -> 0xffe7 (Meta_L -> Option).
    CHECK(EvdevKeyMapper::LookupKeysym(125, false, false) == 0xffeb);
    CHECK(EvdevKeyMapper::LookupKeysym(126, false, false) == 0xffec);
    CHECK(EvdevKeyMapper::LookupKeysym(56, false, false) == 0xffe7);
    CHECK(EvdevKeyMapper::LookupKeysym(100, false, false) == 0xffe8);

    EvdevKeyMapper km;
    std::vector<std::pair<bool, uint32_t>> evs;
    auto rec = [&](bool d, uint32_t ks) { evs.emplace_back(d, ks); };

    // 1. Shift + KEY_1 ('!'), releasing Shift BEFORE releasing KEY_1 still releases '!' cleanly.
    km.Process(42, true, rec);   // Shift_L down
    km.Process(2, true, rec);    // KEY_1 down -> '!'
    km.Process(42, false, rec);  // Shift_L up
    km.Process(2, false, rec);   // KEY_1 up -> '!'
    CHECK(evs.size() == 4);
    CHECK(evs[0] == std::make_pair(true, uint32_t(0xffe1)));
    CHECK(evs[1] == std::make_pair(true, uint32_t('!')));
    CHECK(evs[2] == std::make_pair(false, uint32_t(0xffe1)));
    CHECK(evs[3] == std::make_pair(false, uint32_t('!')));

    // 2. Meta + F24 suppression (Frametop shortcut eaten -> zero Command events sent to macOS).
    evs.clear();
    km.Process(125, true, rec);   // Meta held back
    CHECK(evs.empty());
    km.Process(194, true, rec);   // KEY_F24 cancels held Meta
    km.Process(194, false, rec);
    km.Process(125, false, rec);
    CHECK(evs.empty());

    // 3. Meta solo tap emits Command down + up on release.
    km.Process(125, true, rec);
    CHECK(evs.empty());
    km.Process(125, false, rec);
    CHECK(evs.size() == 2 && evs[0] == std::make_pair(true, uint32_t(0xffeb)) &&
          evs[1] == std::make_pair(false, uint32_t(0xffeb)));

    // 4. Meta + C flushes Command down before 'c' down, and Blur releases held keys.
    evs.clear();
    km.Process(125, true, rec);
    km.Process(46, true, rec);  // KEY_C ('c')
    CHECK(evs.size() == 2 && evs[0] == std::make_pair(true, uint32_t(0xffeb)) &&
          evs[1] == std::make_pair(true, uint32_t('c')));
    evs.clear();
    km.Blur(rec);
    CHECK(evs.size() == 2);

    // 5. Key-name parser: digits never collide with KEY_ESC (1), aliases normalize cleanly.
    uint32_t kc = 0;
    CHECK(ParseEvdevKeyName("1", &kc) && kc == 2);
    CHECK(ParseEvdevKeyName("0", &kc) && kc == 11);
    CHECK(ParseEvdevKeyName("esc", &kc) && kc == 1);
    CHECK(ParseEvdevKeyName("Caps_Lock", &kc) && kc == 58);
    CHECK(ParseEvdevKeyName("left-ctrl", &kc) && kc == 29);
    CHECK(ParseEvdevKeyName("ctrl", &kc) && kc == 29);
    CHECK(ParseEvdevKeyName("cmd", &kc) && kc == 125);
    CHECK(ParseEvdevKeyName("rcmd", &kc) && kc == 126);
    CHECK(ParseEvdevKeyName("f12", &kc) && kc == 88);
    CHECK(ParseEvdevKeyName("code:58", &kc) && kc == 58);
    CHECK(!ParseEvdevKeyName("code:194", &kc));  // F24 reserved internal cancel signal
    CHECK(!ParseEvdevKeyName("none", &kc, false));
    CHECK(ParseEvdevKeyName("none", &kc, true) && kc == 0);
    CHECK(ParseEvdevKeyName("globe", &kc) && kc == EvdevKeyMapper::kEvKeyFn);
    CHECK(ParseEvdevKeyName("fn", &kc) && kc == EvdevKeyMapper::kEvKeyFn);
    CHECK(ParseEvdevKeyName("function", &kc) && kc == EvdevKeyMapper::kEvKeyFn);

    // 6. Full ANSI Dvorak 47-key layout verification: all 26 letters form a bijection onto a..z,
    //    shifted + Caps Lock obey letter-vs-punctuation rules, and moved punctuation is exact.
    std::set<char> dv_letters, dv_letters_upper;
    static constexpr uint32_t kAllPrintableEvdev[] = {
        41, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 43,
        30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
    };
    for (uint32_t ev : kAllPrintableEvdev) {
        const uint32_t lo = EvdevKeyMapper::LookupKeysym(ev, false, false, KeyboardLayout::kDvorak);
        const uint32_t hi = EvdevKeyMapper::LookupKeysym(ev, true, false, KeyboardLayout::kDvorak);
        const uint32_t cap = EvdevKeyMapper::LookupKeysym(ev, false, true, KeyboardLayout::kDvorak);
        CHECK(lo >= 0x20 && lo <= 0x7e && hi >= 0x20 && hi <= 0x7e);
        if (lo >= 'a' && lo <= 'z') {
            dv_letters.insert(char(lo));
            dv_letters_upper.insert(char(hi));
            CHECK(cap == hi);  // Caps Lock uppercases letters
            CHECK(EvdevKeyMapper::LookupKeysym(ev, true, true, KeyboardLayout::kDvorak) == lo);
        } else {
            CHECK(cap == lo);  // Caps Lock never shifts punctuation or digits
        }
    }
    CHECK(dv_letters.size() == 26 && dv_letters_upper.size() == 26);
    CHECK(EvdevKeyMapper::LookupKeysym(12, false, false, KeyboardLayout::kDvorak) == '[');
    CHECK(EvdevKeyMapper::LookupKeysym(12, true, false, KeyboardLayout::kDvorak) == '{');
    CHECK(EvdevKeyMapper::LookupKeysym(13, false, false, KeyboardLayout::kDvorak) == ']');
    CHECK(EvdevKeyMapper::LookupKeysym(13, true, false, KeyboardLayout::kDvorak) == '}');
    CHECK(EvdevKeyMapper::LookupKeysym(16, false, false, KeyboardLayout::kDvorak) == '\'');
    CHECK(EvdevKeyMapper::LookupKeysym(16, true, false, KeyboardLayout::kDvorak) == '"');
    CHECK(EvdevKeyMapper::LookupKeysym(17, false, false, KeyboardLayout::kDvorak) == ',');
    CHECK(EvdevKeyMapper::LookupKeysym(18, false, false, KeyboardLayout::kDvorak) == '.');
    CHECK(EvdevKeyMapper::LookupKeysym(26, false, false, KeyboardLayout::kDvorak) == '/');
    CHECK(EvdevKeyMapper::LookupKeysym(27, false, false, KeyboardLayout::kDvorak) == '=');
    CHECK(EvdevKeyMapper::LookupKeysym(31, false, false, KeyboardLayout::kDvorak) == 'o');
    CHECK(EvdevKeyMapper::LookupKeysym(32, false, false, KeyboardLayout::kDvorak) == 'e');
    CHECK(EvdevKeyMapper::LookupKeysym(33, false, false, KeyboardLayout::kDvorak) == 'u');
    CHECK(EvdevKeyMapper::LookupKeysym(39, false, false, KeyboardLayout::kDvorak) == 's');
    CHECK(EvdevKeyMapper::LookupKeysym(40, false, false, KeyboardLayout::kDvorak) == '-');
    CHECK(EvdevKeyMapper::LookupKeysym(44, false, false, KeyboardLayout::kDvorak) == ';');
    CHECK(EvdevKeyMapper::LookupKeysym(46, false, false, KeyboardLayout::kDvorak) == 'j');
    CHECK(EvdevKeyMapper::LookupKeysym(23, false, false, KeyboardLayout::kDvorak) == 'c');
    CHECK(EvdevKeyMapper::LookupKeysym(53, false, false, KeyboardLayout::kDvorak) == 'z');

    // 7. Dvorak + Caps Lock -> Left Control: no Caps Lock toggle, Control_L ref-counted across
    //    both physical Caps Lock (58) and Left Ctrl (29), and live SetConfig releases held keys.
    KeyboardMappingConfig dv_cfg;
    ApplyKeyboardSettings("dvorak", {{"capslock", "leftctrl"}}, &dv_cfg);
    CHECK(dv_cfg.layout == KeyboardLayout::kDvorak && dv_cfg.keymap.at(58) == 29);

    EvdevKeyMapper dkm(dv_cfg);
    evs.clear();
    dkm.Process(58, true, rec);   // Caps Lock down -> Control_L (0xffe3) down
    dkm.Process(29, true, rec);   // Physical Left Ctrl down -> refcount 2, no duplicate down
    dkm.Process(29, false, rec);  // Physical Left Ctrl up -> refcount 1, Control_L stays held!
    dkm.Process(23, true, rec);   // Physical 'I' -> Dvorak 'c' while Control_L held
    dkm.Process(23, false, rec);
    dkm.Process(58, false, rec);  // Caps Lock up -> Control_L released
    dkm.Process(30, true, rec);   // Physical 'A' -> lowercase 'a' (proving caps_ never toggled!)
    dkm.Process(30, false, rec);
    CHECK(evs.size() == 6);
    CHECK(evs[0] == std::make_pair(true, uint32_t(0xffe3)));
    CHECK(evs[1] == std::make_pair(true, uint32_t('c')));
    CHECK(evs[2] == std::make_pair(false, uint32_t('c')));
    CHECK(evs[3] == std::make_pair(false, uint32_t(0xffe3)));
    CHECK(evs[4] == std::make_pair(true, uint32_t('a')));
    CHECK(evs[5] == std::make_pair(false, uint32_t('a')));

    // 8. dvorak-qwerty-cmd: Dvorak when typing, QWERTY positions while Command is held.
    KeyboardMappingConfig dqc_cfg;
    ApplyKeyboardSettings("dvorak-qwerty-cmd", {{"leftalt", "leftmeta"}}, &dqc_cfg);
    EvdevKeyMapper qkm(dqc_cfg);
    evs.clear();
    qkm.Process(46, true, rec);   // Physical 'C' alone -> Dvorak 'j'
    qkm.Process(46, false, rec);
    qkm.Process(56, true, rec);   // Physical Left Alt -> remapped to Left Meta (held back)
    qkm.Process(46, true, rec);   // Physical 'C' while Meta held -> QWERTY 'c' + flushes Command_L!
    qkm.Process(46, false, rec);
    qkm.Process(56, false, rec);
    CHECK(evs.size() == 6);
    CHECK(evs[0] == std::make_pair(true, uint32_t('j')));
    CHECK(evs[1] == std::make_pair(false, uint32_t('j')));
    CHECK(evs[2] == std::make_pair(true, uint32_t(0xffeb)));
    CHECK(evs[3] == std::make_pair(true, uint32_t('c')));
    CHECK(evs[4] == std::make_pair(false, uint32_t('c')));
    CHECK(evs[5] == std::make_pair(false, uint32_t(0xffeb)));

    // 9. Hot-reloading SetConfig mid-keypress releases held keys and clears stale Caps Lock if remapped.
    EvdevKeyMapper hot;
    evs.clear();
    hot.Process(58, true, rec);   // Toggle Caps Lock ON on default QWERTY
    hot.Process(58, false, rec);
    hot.Process(30, true, rec);   // Hold 'A' (currently 'A' due to Caps Lock)
    evs.clear();
    hot.SetConfig(dv_cfg, rec);   // Releases 'A' and clears caps_ because 58 is now remapped
    CHECK(evs.size() == 1 && evs[0] == std::make_pair(false, uint32_t('A')));
    evs.clear();
    hot.Process(31, true, rec);   // Physical 'S' -> lowercase Dvorak 'o'
    hot.Process(31, false, rec);
    CHECK(evs.size() == 2 && evs[0] == std::make_pair(true, uint32_t('o')));

    // 10. Non-transitive simultaneous remap: Caps Lock -> Left Control AND Left Ctrl -> Globe!
    //     Physical Caps Lock (58) emits Control_L (0xffe3), while physical Left Ctrl (29) emits
    //     macOS Globe (kAppleKeysymGlobe -> kVK_Function = 0x3f) without chaining 58 -> 29 -> 464.
    KeyboardMappingConfig globe_cfg;
    ApplyKeyboardSettings("dvorak", {{"capslock", "leftctrl"}, {"leftctrl", "globe"}}, &globe_cfg);
    CHECK(globe_cfg.keymap.at(58) == 29 && globe_cfg.keymap.at(29) == EvdevKeyMapper::kEvKeyFn);

    EvdevKeyMapper gkm(globe_cfg);
    evs.clear();
    gkm.Process(58, true, rec);   // Physical Caps Lock -> Control_L (0xffe3) down
    gkm.Process(58, false, rec);  // Physical Caps Lock -> Control_L up
    gkm.Process(29, true, rec);   // Physical Left Ctrl -> Globe (kAppleKeysymGlobe) down
    gkm.Process(32, true, rec);   // Physical 'D' -> Dvorak 'e' (Globe+E emoji shortcut!)
    gkm.Process(32, false, rec);
    gkm.Process(29, false, rec);  // Physical Left Ctrl -> Globe up
    CHECK(evs.size() == 6);
    CHECK(evs[0] == std::make_pair(true, uint32_t(0xffe3)));
    CHECK(evs[1] == std::make_pair(false, uint32_t(0xffe3)));
    CHECK(evs[2] == std::make_pair(true, kAppleKeysymGlobe));
    CHECK(evs[3] == std::make_pair(true, uint32_t('e')));
    CHECK(evs[4] == std::make_pair(false, uint32_t('e')));
    CHECK(evs[5] == std::make_pair(false, kAppleKeysymGlobe));
    uint16_t vk_out = 0;
    CHECK(IsAppleMacKeycodeKeysym(evs[2].second, &vk_out) && vk_out == kMacVkFunction);
    CHECK(!IsAppleMacKeycodeKeysym(0xffe3, &vk_out));
}

void TestRfc3640AudioAndCredsStore() {
    // Single-AU RFC 3640 packet: [0x00, 0x10, (5 << 3), 0xaa, 0xbb, 0xcc, 0xdd, 0xee]
    const std::vector<uint8_t> single_pkt = {0x00, 0x10, 0x00, 0x28, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    std::vector<std::vector<uint8_t>> aus;
    ParseRfc3640AuPayloads(single_pkt.data(), single_pkt.size(), &aus);
    CHECK(aus.size() == 1 && aus[0] == std::vector<uint8_t>({0xaa, 0xbb, 0xcc, 0xdd, 0xee}));

    // Multi-AU RFC 3640 packet: 2 AUs of 3 bytes and 2 bytes (hdr_bits = 32 = 0x0020)
    aus.clear();
    const std::vector<uint8_t> multi_pkt = {0x00, 0x20, 0x00, 0x18, 0x00, 0x10, 1, 2, 3, 4, 5};
    ParseRfc3640AuPayloads(multi_pkt.data(), multi_pkt.size(), &aus);
    CHECK(aus.size() == 2 && aus[0] == std::vector<uint8_t>({1, 2, 3}) && aus[1] == std::vector<uint8_t>({4, 5}));

    // Private 0600 credential file roundtrip + permission enforcement
    char tmp_dir[] = "/tmp/ft-apple-creds-XXXXXX";
    CHECK(mkdtemp(tmp_dir) != nullptr);
    const std::string cpath = AppleCredsPath("Studio-Mac.local", tmp_dir);
    CHECK(SaveAppleCreds(cpath, "alice", "s3cr3t-test-value", AuthMode::kNonSrp));
    struct stat st{};
    CHECK(stat(cpath.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    std::string u, p;
    AuthMode mode = AuthMode::kAuto;
    CHECK(LoadAppleCreds(cpath, &u, &p, &mode) && u == "alice" && p == "s3cr3t-test-value");
    CHECK(mode == AuthMode::kNonSrp);
    CHECK(!SaveAppleCreds(cpath, "alice", "x", AuthMode::kAuto));
    // A file from before the mode line counts as SRP, so nothing falls back to RSA.
    {
        const int wfd = open(cpath.c_str(), O_WRONLY | O_TRUNC);
        CHECK(wfd >= 0 && write(wfd, "bob\npw\n", 7) == 7);
        close(wfd);
    }
    CHECK(LoadAppleCreds(cpath, &u, &p, &mode) && u == "bob" && p == "pw" && mode == AuthMode::kSrp);
    CHECK(StoredAuthMode(cpath) == AuthMode::kSrp);
    // World-readable file must be rejected by LoadAppleCreds
    chmod(cpath.c_str(), 0644);
    CHECK(!LoadAppleCreds(cpath, &u, &p));
    CHECK(StoredAuthMode(cpath) == AuthMode::kSrp);  // there but unreadable: fail closed
    unlink(cpath.c_str());
    CHECK(StoredAuthMode(cpath) == AuthMode::kAuto);  // a new host
    CHECK(SaveAppleCreds(cpath, "alice", "pw", AuthMode::kNonSrp) && StoredAuthMode(cpath) == AuthMode::kNonSrp);
    unlink(cpath.c_str());

    // Signing in again: a Mac stored as srp uses SRP only; a new or nonsrp one may fall back.
    CHECK(SignInMode(AuthMode::kSrp) == AuthMode::kSrp);
    CHECK(SignInMode(AuthMode::kAuto) == AuthMode::kAuto);
    CHECK(SignInMode(AuthMode::kNonSrp) == AuthMode::kAuto);
    // What's saved: the mode that worked; with --no-verify the stored one, SRP for a new host.
    CHECK(ModeToSave(AuthMode::kAuto, true, "nonsrp") == AuthMode::kNonSrp);
    CHECK(ModeToSave(AuthMode::kNonSrp, true, "srp") == AuthMode::kSrp);
    CHECK(ModeToSave(AuthMode::kAuto, false, "") == AuthMode::kSrp);
    CHECK(ModeToSave(AuthMode::kSrp, false, "") == AuthMode::kSrp);
    CHECK(ModeToSave(AuthMode::kNonSrp, false, "") == AuthMode::kNonSrp);
    rmdir(tmp_dir);
}

void TestSleepWakeAndIdleLivenessTimeout() {
    using Action = SessionLiveness::Action;
    constexpr int64_t kMs = 1'000'000LL;

    // 1. Steady stream with periodic traffic stays healthy indefinitely.
    SessionLiveness lv;
    lv.Reset(1000 * kMs, 1000 * kMs, 3000, 4000, 6000, 15000);
    CHECK(lv.Step(2000 * kMs, 2000 * kMs, true) == Action::kOk);
    CHECK(!lv.wake_probe_pending());

    // 2. Deep suspend (boottime advances 90 min while monotonic advances 4 ms):
    //    even if a stale pre-sleep packet is sitting in the kernel socket buffer
    //    (got_rx == true on the wake tick), Step() must probe and enter wake grace.
    const int64_t mono_wake = 2004 * kMs;
    const int64_t boot_wake = (2000 + 5400'000) * kMs;
    CHECK(lv.Step(mono_wake, boot_wake, true) == Action::kProbeWake);
    CHECK(lv.wake_probe_pending());

    // 3. No fresh post-wake packet within 3.9 s -> still within wake grace; at 4.0 s -> timeout.
    CHECK(lv.Step(mono_wake + 3900 * kMs, boot_wake + 3900 * kMs, false) == Action::kOk);
    CHECK(lv.Step(mono_wake + 4000 * kMs, boot_wake + 4000 * kMs, false) == Action::kTimedOutWake);

    // 4. Short suspend where the remote session survived: fresh packet after wake clears grace.
    lv.Reset(1000 * kMs, 1000 * kMs, 3000, 4000, 6000, 15000);
    CHECK(lv.Step(1004 * kMs, 6004 * kMs, false) == Action::kProbeWake);
    CHECK(lv.wake_probe_pending());
    CHECK(lv.Step(1050 * kMs, 6050 * kMs, true) == Action::kOk);
    CHECK(!lv.wake_probe_pending());
    CHECK(lv.Step(5500 * kMs, 10500 * kMs, false) == Action::kOk);

    // 5. Silent peer (e.g. Mac slept without sending TCP FIN): probe once at 6 s, timeout at 15 s.
    lv.Reset(1000 * kMs, 1000 * kMs, 3000, 4000, 6000, 15000);
    CHECK(lv.Step(6999 * kMs, 6999 * kMs, false) == Action::kOk);
    CHECK(lv.Step(7000 * kMs, 7000 * kMs, false) == Action::kProbeIdle);
    CHECK(lv.Step(12000 * kMs, 12000 * kMs, false) == Action::kOk);  // probe does not repeat
    CHECK(lv.Step(16000 * kMs, 16000 * kMs, false) == Action::kTimedOutIdle);

    // 6. Traffic after idle probe resets both the probe flag and timeout.
    lv.Reset(1000 * kMs, 1000 * kMs, 3000, 4000, 6000, 15000);
    CHECK(lv.Step(7000 * kMs, 7000 * kMs, false) == Action::kProbeIdle);
    CHECK(lv.Step(8000 * kMs, 8000 * kMs, true) == Action::kOk);
    CHECK(lv.Step(13999 * kMs, 13999 * kMs, false) == Action::kOk);
    CHECK(lv.Step(14000 * kMs, 14000 * kMs, false) == Action::kProbeIdle);

    // 7. Live loopback half-open peer timeout: server accepts handshake and keeps TCP open
    //    while sending zero UDP/TCP media; PollStep() must time out cleanly so ft-screens reconnects.
    const int srv_listen = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(srv_listen >= 0);
    int one = 1;
    setsockopt(srv_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    CHECK(bind(srv_listen, reinterpret_cast<sockaddr *>(&sin), sizeof sin) == 0);
    CHECK(listen(srv_listen, 4) == 0);
    socklen_t slen = sizeof sin;
    CHECK(getsockname(srv_listen, reinterpret_cast<sockaddr *>(&sin), &slen) == 0);
    const uint16_t tcp_port = ntohs(sin.sin_port);

    const int probe_u = socket(AF_INET, SOCK_DGRAM, 0);
    sin.sin_port = 0;
    CHECK(bind(probe_u, reinterpret_cast<sockaddr *>(&sin), sizeof sin) == 0);
    CHECK(getsockname(probe_u, reinterpret_cast<sockaddr *>(&sin), &slen) == 0);
    uint16_t cli_udp_base = ntohs(sin.sin_port);
    if (cli_udp_base >= 65530) cli_udp_base -= 10;
    close(probe_u);

    UniquePkey rsa(EVP_RSA_gen(2048));
    CHECK(rsa != nullptr);
    const auto spki = EncodeDerSpki(rsa.get());

    std::atomic<bool> stop_srv{false};
    std::thread srv([&] {
        const int cfd = accept(srv_listen, nullptr, nullptr);
        if (cfd < 0) return;
        const std::string banner = "RFB 003.889\n";
        SendAll(cfd, reinterpret_cast<const uint8_t *>(banner.data()), banner.size());
        uint8_t cver[12];
        RecvExact(cfd, cver, 12);
        const uint8_t types[2] = {1, 33};
        uint8_t rsa_init[15];
        if (!SendAll(cfd, types, 2) || !RecvExact(cfd, rsa_init, 15)) {
            close(cfd);
            return;
        }
        std::vector<uint8_t> spkt = {0x00, 0x00};
        AppendBe32(spkt, uint32_t(spki.size()));
        spkt.insert(spkt.end(), spki.begin(), spki.end());
        std::vector<uint8_t> spkt_w;
        AppendBe32(spkt_w, uint32_t(spkt.size()));
        spkt_w.insert(spkt_w.end(), spkt.begin(), spkt.end());
        SendAll(cfd, spkt_w);

        uint8_t auth_hdr[4];
        RecvExact(cfd, auth_hdr, 4);
        const uint32_t auth_len = ReadBe32(auth_hdr);
        std::vector<uint8_t> auth_body(auth_len);
        RecvExact(cfd, auth_body.data(), auth_len);
        std::vector<uint8_t> wrap_key;
        if (!RsaPkcs1Decrypt(rsa.get(), auth_body.data() + 8 + 128 + 2, 256, &wrap_key) || wrap_key.size() != 16) {
            close(cfd);
            return;
        }
        const uint8_t ok_res[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        SendAll(cfd, ok_res, 8);

        uint8_t cinit = 0;
        RecvExact(cfd, &cinit, 1);
        std::vector<uint8_t> sinit(28, 0);
        WriteBe16(&sinit[0], 1920);
        WriteBe16(&sinit[2], 1080);
        WriteBe32(&sinit[20], 4);
        std::memcpy(&sinit[24], "Test", 4);
        SendAll(cfd, sinit);

        // Read ViewerInfo (66B) + 0x12 followup (12B) + SetEncodings (56B) (no 0x1d since virtual_display=false).
        std::vector<uint8_t> pre(66 + 12 + 56);
        if (!RecvExact(cfd, pre.data(), pre.size())) {
            close(cfd);
            return;
        }

        uint8_t cbc_key[16], cbc_iv[16], rekey[36]{};
        RandomBytes(cbc_key, 16);
        RandomBytes(cbc_iv, 16);
        WriteBe32(rekey, 1);
        Aes128EcbCrypt(true, wrap_key.data(), cbc_key, 16, rekey + 4);
        Aes128EcbCrypt(true, wrap_key.data(), cbc_iv, 16, rekey + 20);
        StreamCipher srv_cipher;
        srv_cipher.InitFromRekey0x44f(rekey, wrap_key.data());

        std::vector<uint8_t> fbu = {0x00, 0x00, 0x00, 0x01};
        fbu.insert(fbu.end(), 8, 0);
        AppendBe32(fbu, 1103);
        fbu.insert(fbu.end(), rekey, rekey + 36);
        SendAll(cfd, fbu);

        uint8_t tog[8];
        RecvExact(cfd, tog, 8);

        // Drain SetEncodings and 0x1c, answer with canvas 1920x1080, then go silent while keeping TCP open!
        for (int k = 0; k < 8; ++k) {
            uint8_t lbuf[2];
            if (!RecvExact(cfd, lbuf, 2, 1000)) break;
            const uint16_t clen = ReadBe16(lbuf);
            std::vector<uint8_t> body(clen);
            if (!RecvExact(cfd, body.data(), clen, 1000)) break;
            std::vector<uint8_t> msg;
            if (!srv_cipher.DecryptRecordBody(body.data(), body.size(), &msg)) break;
            if (!msg.empty() && msg[0] == 0x1c) {
                std::vector<uint8_t> wire;
                srv_cipher.EncryptMessage(BuildMock0x1cAnswer(1920, 1080, 1), &wire);
                SendAll(cfd, wire);
                break;
            }
        }
        while (!stop_srv.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        close(cfd);
    });

    SessionOptions opt;
    opt.host = "127.0.0.1";
    opt.port = tcp_port;
    opt.bind_ip = "127.0.0.1";
    opt.udp_ctrl_port = cli_udp_base;
    opt.udp_video_port = uint16_t(cli_udp_base + 1);
    opt.username = "testuser";
    opt.password = "pw";
    opt.auth_mode = AuthMode::kNonSrp;
    opt.virtual_display = false;
    opt.warmup_tcp = false;
    opt.settle_ms = 5;
    opt.toggle_settle_ms = 5;
    opt.rx_probe_ms = 60;
    opt.rx_timeout_ms = 160;

    SessionClient client;
    std::string err;
    CHECK(client.Connect(opt, &err));
    std::vector<H264RtpAssembler::AccessUnit> aus;
    bool timed_out = false;
    for (int i = 0; i < 40; ++i) {
        if (!client.PollStep(15, &aus, &err)) {
            timed_out = true;
            break;
        }
    }
    CHECK(timed_out);
    CHECK(err.find("no media from host") != std::string::npos);

    stop_srv = true;
    client.Close();
    srv.join();
    close(srv_listen);
}

}  // namespace

// A screensharingd stand-in for the sign-in rules, one scripted answer per connection:
// "short" answers SRP's first packet with a short length and hangs up (a Mac without SRP),
// "reject" sends a real RFC 5054 challenge and then SecurityResult 1 (a wrong password),
// "nonsrp-ok" takes a non-SRP login.
struct ScriptedAuthRun {
    bool ok = false;
    bool srp_not_offered = false;
    bool extra_connection = false;  // the client connected again after the script ran out
    std::string auth_used;
    std::vector<std::string> kinds;  // each connection's first auth packet: srp or nonsrp
    // Joined, to compare with a string literal: CHECK can't take a braced list's commas.
    std::string kinds_said() const {
        std::string out;
        for (const std::string &k : kinds) out += (out.empty() ? "" : " ") + k;
        return out;
    }
};

bool RecvFramed(int fd, std::vector<uint8_t> *body) {
    uint8_t len_b[4];
    if (!RecvExact(fd, len_b, 4, 5000)) return false;
    const uint32_t len = ReadBe32(len_b);
    if (len > 65536) return false;
    body->resize(len);
    return len == 0 || RecvExact(fd, body->data(), len, 5000);
}

std::vector<uint8_t> BuildRfcChallenge() {
    BIGNUM *raw_N = nullptr;
    CHECK(BN_hex2bn(&raw_N, kRfc5054Prime4096Hex) > 0);
    UniqueBn bn_N(raw_N), bn_g(BN_new()), bn_b(BN_new()), bn_B(BN_new());
    UniqueBnCtx ctx(BN_CTX_new());
    BN_set_word(bn_g.get(), 5);
    BN_set_word(bn_b.get(), 987654321u);
    CHECK(BN_mod_exp(bn_B.get(), bn_g.get(), bn_b.get(), bn_N.get(), ctx.get()) == 1);
    std::array<uint8_t, kSrpModulusLen> N_bytes{}, B_pad{};
    BN_bn2binpad(bn_N.get(), N_bytes.data(), kSrpModulusLen);
    BN_bn2binpad(bn_B.get(), B_pad.data(), kSrpModulusLen);
    const std::string cap = "mda=SHA-512,replay_detection,conf+int=ChaCha20-Poly1305,kdf=SALTED-SHA512-PBKDF2";
    std::vector<uint8_t> c(13, 0);
    c.insert(c.end(), N_bytes.begin(), N_bytes.end());
    AppendBe16(c, 1);
    c.push_back(5);
    c.push_back(32);
    c.insert(c.end(), 32, 0x42);
    AppendBe16(c, kSrpModulusLen);
    c.insert(c.end(), B_pad.begin(), B_pad.end());
    AppendBe64(c, 1000);
    AppendBe16(c, uint16_t(cap.size()));
    c.insert(c.end(), cap.begin(), cap.end());
    return c;
}

bool ServeScriptedAuth(int cfd, const std::string &what, const std::vector<uint8_t> &key_wire,
                       const std::vector<uint8_t> &challenge, std::string *kind) {
    const uint8_t ver[12] = {'R', 'F', 'B', ' ', '0', '0', '3', '.', '8', '8', '9', '\n'};
    const uint8_t types[2] = {1, 33};
    uint8_t cver[12], rsa_init[15];
    std::vector<uint8_t> body;
    if (!SendAll(cfd, ver, 12) || !RecvExact(cfd, cver, 12, 5000) || !SendAll(cfd, types, 2) ||
        !RecvExact(cfd, rsa_init, 15, 5000) || !SendAll(cfd, key_wire) || !RecvFramed(cfd, &body) ||
        body.size() < 8)
        return false;
    *kind = body[7] == 0x02 ? "srp" : "nonsrp";
    std::vector<uint8_t> reply;
    if (what == "short") {
        AppendBe32(reply, 8);
        return SendAll(cfd, reply);
    }
    if (what == "nonsrp-ok") {
        reply.assign(8, 0);
        return SendAll(cfd, reply);
    }
    AppendBe32(reply, uint32_t(challenge.size()));
    reply.insert(reply.end(), challenge.begin(), challenge.end());
    if (!SendAll(cfd, reply) || !RecvFramed(cfd, &body)) return false;
    std::vector<uint8_t> result;
    AppendBe32(result, 0);  // no server proof
    AppendBe32(result, 1);  // SecurityResult: failed
    return SendAll(cfd, result);
}

ScriptedAuthRun RunScriptedAuth(AuthMode mode, const std::vector<std::string> &script, EVP_PKEY *srv_rsa,
                                const std::vector<uint8_t> &challenge) {
    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(lfd >= 0);
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(lfd, reinterpret_cast<const sockaddr *>(&sin), sizeof sin) == 0);
    socklen_t slen = sizeof sin;
    CHECK(getsockname(lfd, reinterpret_cast<sockaddr *>(&sin), &slen) == 0);
    CHECK(listen(lfd, 4) == 0);

    const auto spki = EncodeDerSpki(srv_rsa);
    std::vector<uint8_t> key_pkt = {0x00, 0x00};
    AppendBe32(key_pkt, uint32_t(spki.size()));
    key_pkt.insert(key_pkt.end(), spki.begin(), spki.end());
    std::vector<uint8_t> key_wire;
    AppendBe32(key_wire, uint32_t(key_pkt.size()));
    key_wire.insert(key_wire.end(), key_pkt.begin(), key_pkt.end());

    ScriptedAuthRun run;
    std::thread server([&]() {
        for (const std::string &what : script) {
            pollfd pfd{lfd, POLLIN, 0};
            if (poll(&pfd, 1, 5000) <= 0) return;
            const int cfd = accept(lfd, nullptr, nullptr);
            if (cfd < 0) return;
            std::string kind;
            if (ServeScriptedAuth(cfd, what, key_wire, challenge, &kind)) run.kinds.push_back(kind);
            close(cfd);
        }
    });
    SessionOptions opt;
    opt.host = "127.0.0.1";
    opt.port = ntohs(sin.sin_port);
    opt.username = "alice";
    opt.password = "correct-horse-battery-staple";
    opt.auth_mode = mode;
    SessionClient client;
    std::string err;
    run.ok = client.VerifyCredentials(opt, &err);
    server.join();
    pollfd pfd{lfd, POLLIN, 0};
    run.extra_connection = poll(&pfd, 1, 0) > 0;
    close(lfd);
    run.srp_not_offered = client.srp_not_offered();
    run.auth_used = client.auth_used();
    return run;
}

void TestAuthFallbackRules() {
    UniquePkey srv_rsa(EVP_RSA_gen(2048));
    CHECK(srv_rsa != nullptr);
    const auto challenge = BuildRfcChallenge();

    // First sign-in, a Mac without SRP: SRP's first packet gets a short reply, then RSA.
    auto run = RunScriptedAuth(AuthMode::kAuto, {"short", "nonsrp-ok"}, srv_rsa.get(), challenge);
    CHECK(run.ok && run.auth_used == "nonsrp" && run.srp_not_offered);
    CHECK(run.kinds_said() == "srp nonsrp" && !run.extra_connection);

    // A wrong password over SRP is never sent again over RSA.
    run = RunScriptedAuth(AuthMode::kAuto, {"reject"}, srv_rsa.get(), challenge);
    CHECK(!run.ok && !run.srp_not_offered && run.auth_used.empty());
    CHECK(run.kinds_said() == "srp" && !run.extra_connection);

    // A stored SRP login never falls back, even when the server claims to have no SRP.
    run = RunScriptedAuth(AuthMode::kSrp, {"short"}, srv_rsa.get(), challenge);
    CHECK(!run.ok && run.srp_not_offered && run.auth_used.empty());
    CHECK(run.kinds_said() == "srp" && !run.extra_connection);

    // A stored non-SRP login goes straight to RSA.
    run = RunScriptedAuth(AuthMode::kNonSrp, {"nonsrp-ok"}, srv_rsa.get(), challenge);
    CHECK(run.ok && run.auth_used == "nonsrp" && run.kinds_said() == "nonsrp" && !run.extra_connection);
}

// A media blob whose trailer points the offset table far past the end must be
// refused, not read: off_tbl + num_objs * off_sz used to wrap around.
void TestBplistBounds() {
    std::vector<uint8_t> blob(48, 0);
    std::memcpy(blob.data(), "bplist00", 8);
    uint8_t *tr = blob.data() + blob.size() - 32;
    tr[6] = 8;  // offset size
    tr[7] = 1;  // ref size
    WriteBe64(tr + 8, 1);                      // num_objs
    WriteBe64(tr + 16, 0);                     // top object
    WriteBe64(tr + 24, 0xFFFFFFFFFFFFFFF8ull); // offset table
    std::vector<uint8_t> out;
    CHECK(!ParseBplistMediaBlob(blob.data(), blob.size(), &out));
    WriteBe64(tr + 24, 9);
    WriteBe64(tr + 8, 4096);  // more offsets than fit before the trailer
    CHECK(!ParseBplistMediaBlob(blob.data(), blob.size(), &out));
}

int main() {
    TestBplistBounds();
    TestBplistAndNegotiatorOffers();
    TestSrpAndNonSrpAuth();
    TestAuthFallbackRules();
    TestEnc1103StreamCipher();
    TestDisplayConfig0x1dAnd0x451();
    TestSrtpAndSrtcp();
    TestAvcConfig0x92AndSpsPatch();
    TestH264RtpAssembler();
    TestZeroDelayPoc2Rewrite();
    TestInputKeyMappingAndMetaHoldback();
    TestRfc3640AudioAndCredsStore();
    TestSleepWakeAndIdleLivenessTimeout();
    TestFullLoopbackSession();
    std::printf("apple_hp_test: all %d checks passed\n", g_checks);
    return 0;
}
