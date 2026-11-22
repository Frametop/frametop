// One connection to ft-npud for the NPU backends (NpuLandmarks, NpuDetector): the daemon serves one client at a
// time, so they share it. ensure() connects, reading the heads file afresh (after a failure it tries again only
// every kRetrySeconds), and settles a posted detector request, so the crop slots are free when it returns true.
// One thread: the tracker calls its backends in turn.
#pragma once

#include "npuclient.h"
#include "npuheads.h"

#include <chrono>
#include <string>

// Where an NPU backend's time went, summed over its requests (ms): its CPU crops, the time it waited for the
// reply (a request run at once: the round trip), ft-npud's own time and the DSP's.
struct NpuTimes {
    int requests = 0;
    double crop = 0, wait = 0, daemon = 0, dsp = 0;
    void add(double crop_ms, double wait_ms, const NpuClient &cl) {
        ++requests, crop += crop_ms, wait += wait_ms, daemon += cl.daemon_ms(), dsp += cl.wrapper_ms();
    }
    // "3.07 ms DSP, 5.27 in ft-npud, 5.38 waited, 2.92 crops a request"
    std::string summary() const;
};

class NpuSession {
public:
    static constexpr double kRetrySeconds = 2.0;
    static constexpr int kTimeoutMs = 50;
    explicit NpuSession(std::string dir) : dir_(std::move(dir)) {}
    bool ensure();
    // Counts connections: a posted request's outputs are only there for the connection that posted it.
    int connection() const { return connection_; }
    // A failed request (the client has closed): counted and logged; the backends fall back to the CPU.
    void fail(const std::string &why);
    bool connected() const { return client_.connected(); }
    NpuClient &client() { return client_; }
    const NpuHeads &heads() const { return heads_; }
    const std::string &last_error() const { return err_; }
    int failures = 0;

private:
    std::string dir_, err_;
    int connection_ = 0;
    NpuHeads heads_;
    NpuClient client_;
    std::chrono::steady_clock::time_point retry_at_{};
};
