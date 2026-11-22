// The client side of ft-npud (npu-hands npud/ft_npud.py), the root daemon that runs the NPU student: its
// UID-owned shared memory (a 64-byte header, 4 crop slots, 4 output slots) and its SOCK_SEQPACKET doorbell (send
// the crop count, receive one status byte). One request at a time; any failure or timeout closes the connection,
// so a late reply can never be read as the next one's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class NpuClient {
public:
    static constexpr int kCrop = 224, kMaxCrops = 4;
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

private:
    static constexpr size_t kHeader = 64, kCrops = 4096, kOuts = kCrops + size_t(kMaxCrops) * kCrop * kCrop;
    int sock_ = -1;
    uint8_t *shm_ = nullptr;
    size_t shm_bytes_ = 0, out_bytes_ = 0;
};
