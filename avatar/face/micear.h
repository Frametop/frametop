// The wearer's microphone, heard: the session's default source (on the Frame it runs through
// echo cancelling), mono, as samples to take when wanted. What is heard stays in this program's
// memory for the half second it takes to be asked for: nothing is written down or sent.
//
// A thread of its own records (libpulse-simple); the calls here never wait for it.
#pragma once

#include <cstddef>
#include <memory>

namespace micear {

constexpr int kRate = 16000;   // samples a second: enough to tell vowels by

class Ear {
public:
    ~Ear() { Close(); }
    // Starts hearing, as `name` in the mixer. It returns at once: a microphone that can't be
    // recorded is a line on stderr and an ear that hears nothing.
    void Open(const char *name);
    // What was heard since the last call, oldest first, -1 to 1: up to `most` samples (when
    // more are waiting, the newest). Never waits.
    size_t Take(float *out, size_t most);
    bool hearing() const;   // the recording is up
    void Close();

private:
    struct Heard;
    std::shared_ptr<Heard> heard_;   // the recording thread's too: it may outlive the ear
};

}  // namespace micear
