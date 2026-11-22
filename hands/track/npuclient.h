// The client side of ft-npud (npu-hands npud/ft_npud.py), the root daemon that runs the NPU student and, when
// it serves one, the palm detector: its UID-owned shared memory (a 64-byte header, 4 crop slots, 4 landmark output
// slots, 4 detector output slots) and its SOCK_SEQPACKET doorbell (send the crop count, kDet | count for the
// detector, receive one status byte). One request at a time; any failure or timeout closes the connection, so a
// late reply can never be read as the next one's. A detector request can also be posted and settled later
// (post_det, settle): the crop slots then belong to it until settle(), and any other request settles it first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class NpuClient {
public:
    static constexpr int kCrop = 224, kMaxCrops = 4;
    static constexpr int kDetGrid = 28;                                  // the detector's outputs: 28 x 28 cells
    static constexpr size_t kDetBytes = size_t(5) * kDetGrid * kDetGrid; // obj s8 [1][28][28], reg s8 [4][28][28]
    NpuClient() = default;
    NpuClient(const NpuClient &) = delete;
    NpuClient &operator=(const NpuClient &) = delete;
    ~NpuClient() { close(); }

    // DIR/ft-npud.shm and DIR/ft-npud.sock. out_bytes: one crop's output size (NpuHeads::out_bytes);
    // graph_id: the heads file's, which must equal the daemon's.
    bool open(const std::string &dir, size_t out_bytes, uint32_t graph_id, std::string &err);
    void close();
    bool connected() const { return sock_ >= 0; }
    // The i-th crop slot (kCrop x kCrop gray bytes), written before run().
    uint8_t *crop(int i) { return shm_ + kCrops + size_t(i) * kCrop * kCrop; }
    // n crops in, n outputs out (output(i)); false (and closed) on a refusal, timeout or lost daemon.
    bool run(int n, int timeout_ms, std::string &err);
    const uint8_t *output(int i) const { return shm_ + kOuts + size_t(i) * out_bytes_; }
    float wrapper_ms() const;   // the DSP's time for the last request
    float daemon_ms() const;    // ft-npud's own time for it, request received to outputs published
    // The detector (has_detector()): n whole-image crops in (post_det, then settle), det_output(i) out (obj then
    // reg; value = q * scale).
    bool has_detector() const;
    const int8_t *det_output(int i) const {
        return reinterpret_cast<const int8_t *>(shm_ + det_outs() + size_t(i) * kDetBytes);
    }
    float det_scale(int which) const;   // 0: obj's, 1: reg's
    // post_det rings the doorbell and returns; settle waits for its reply (true at once when nothing is in
    // flight). Write no crop slot in between.
    bool post_det(int n, std::string &err);
    bool settle(int timeout_ms, std::string &err);
    bool in_flight() const { return in_flight_; }

private:
    static constexpr size_t kHeader = 64, kCrops = 4096, kOuts = kCrops + size_t(kMaxCrops) * kCrop * kCrop;
    static constexpr uint8_t kDet = 0x10;
    static constexpr uint32_t kFeatDet = 1;
    size_t det_outs() const { return kOuts + size_t(kMaxCrops) * out_bytes_; }
    bool request(uint8_t req, int timeout_ms, std::string &err);
    bool post(uint8_t req, std::string &err);
    float header_float(int word) const;
    int sock_ = -1;
    bool in_flight_ = false;
    uint8_t *shm_ = nullptr;
    size_t shm_bytes_ = 0, out_bytes_ = 0;
};
