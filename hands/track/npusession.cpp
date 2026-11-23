#include "npusession.h"
#include "npuserver.h"

#include <cstdio>

bool NpuSession::ensure() {
    if (client_.connected()) {
        std::string e;
        if (!client_.settle(kTimeoutMs, e)) return fail(e), false;
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < retry_at_) return false;
    retry_at_ = now + std::chrono::milliseconds(int(kRetrySeconds * 1000));
    std::string e;
    const std::string dir = npu_server_dir(dir_);   // /run/host + dir_ inside the container
    if (!heads_.load(dir + "/ft-npud.heads", e) || !client_.open(dir, heads_.out_bytes(), heads_.graph_id, e)) {
        if (e != err_) std::fprintf(stderr, "npu: %s; the CPU models instead\n", e.c_str());
        err_ = e;
        return false;
    }
    std::fprintf(stderr, "npu: ft-npud connected (graph %08x%s)\n", heads_.graph_id,
                 client_.has_detector() ? ", detector" : "");
    err_.clear();
    ++connection_;
    return true;
}

void NpuSession::fail(const std::string &why) {
    ++failures;
    err_ = why;
    // the client has closed: wait the retry time before connecting again, so a wedged daemon costs one timeout
    // per kRetrySeconds and not one a step
    retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(kRetrySeconds * 1000));
    std::fprintf(stderr, "npu: %s; the CPU models until ft-npud answers again\n", why.c_str());
}

std::string NpuTimes::summary() const {
    const double n = std::max(requests, 1);
    char b[160];
    std::snprintf(b, sizeof b, "%.2f ms DSP, %.2f in ft-npud, %.2f waited, %.2f cropping", dsp / n,
                  daemon / n, wait / n, crop / n);
    return b;
}
