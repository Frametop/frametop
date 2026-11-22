// The NPU student as a LandmarkBatch: Nets' own crops go to ft-npud (up to 4 a request), and NpuHeads decodes the
// outputs on the CPU into the same Landmarks the stock and fine-tuned ncnn models give. When the daemon is
// missing, serves another graph or fails a request, run() returns false (the tracker uses the CPU model) and the
// daemon is tried again after kRetrySeconds, reading its heads file afresh.
#pragma once

#include "landmarkbatch.h"
#include "npuclient.h"
#include "npuheads.h"

#include <chrono>
#include <string>

class NpuLandmarks : public LandmarkBatch {
public:
    static constexpr double kRetrySeconds = 2.0;
    static constexpr int kTimeoutMs = 50;
    NpuLandmarks(const Nets &nets, std::string dir) : nets_(nets), dir_(std::move(dir)) {}
    bool run(const std::vector<const Image *> &images, const std::vector<Roi> &rois,
             std::vector<Landmarks> &out) override;
    bool connected() const { return client_.connected(); }
    const std::string &last_error() const { return err_; }
    int requests = 0, failures = 0;
    double dsp_ms = 0;   // summed DSP time of the requests

private:
    bool ensure();
    void fail(const std::string &why);
    const Nets &nets_;
    std::string dir_, err_;
    NpuHeads heads_;
    NpuClient client_;
    std::chrono::steady_clock::time_point retry_at_{};
};
