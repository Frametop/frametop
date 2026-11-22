// One connection to ft-npud for the NPU backends (NpuLandmarks, NpuDetector): the daemon serves one client at a
// time, so they share it. ensure() connects, reading the heads file afresh; after a failure it tries again only
// every kRetrySeconds. One thread: the tracker calls its backends in turn.
#pragma once

#include "npuclient.h"
#include "npuheads.h"

#include <chrono>
#include <string>

class NpuSession {
public:
    static constexpr double kRetrySeconds = 2.0;
    static constexpr int kTimeoutMs = 50;
    explicit NpuSession(std::string dir) : dir_(std::move(dir)) {}
    bool ensure();
    // A failed request (the client has closed): counted and logged; the backends fall back to the CPU.
    void fail(const std::string &why);
    bool connected() const { return client_.connected(); }
    NpuClient &client() { return client_; }
    const NpuHeads &heads() const { return heads_; }
    const std::string &last_error() const { return err_; }
    int failures = 0;

private:
    std::string dir_, err_;
    NpuHeads heads_;
    NpuClient client_;
    std::chrono::steady_clock::time_point retry_at_{};
};
