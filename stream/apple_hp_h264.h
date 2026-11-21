// Apple High Performance Screen Sharing client (MIT), part of apple_hp.h.
// H.264 bitstream surgery (POC type 2, no reorder), the 0x92 avcC extractor and the RTP assembler.
#pragma once

#include "apple_hp_rtp.h"  // SrtpContext::RtpHeader, which the assembler takes

namespace ft_apple_hp {

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

}  // namespace ft_apple_hp
