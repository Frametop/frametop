#include "npusession.h"

#include <cstdio>

bool NpuSession::ensure() {
    if (client_.connected()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < retry_at_) return false;
    retry_at_ = now + std::chrono::milliseconds(int(kRetrySeconds * 1000));
    std::string e;
    if (!heads_.load(dir_ + "/ft-npud.heads", e) || !client_.open(dir_, heads_.out_bytes(), heads_.graph_id, e)) {
        if (e != err_) std::fprintf(stderr, "npu: %s; the CPU models instead\n", e.c_str());
        err_ = e;
        return false;
    }
    std::fprintf(stderr, "npu: ft-npud connected (graph %08x%s)\n", heads_.graph_id,
                 client_.has_detector() ? ", detector" : "");
    err_.clear();
    return true;
}

void NpuSession::fail(const std::string &why) {
    ++failures;
    err_ = why;
    std::fprintf(stderr, "npu: %s; the CPU models until ft-npud answers again\n", why.c_str());
}
