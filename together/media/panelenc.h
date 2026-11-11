// One shared panel's encoder with the pictures it reads (irisenc.h, pictures.h): which picture
// is free to draw into, which the encoder still reads, and the one queued last, kept as it is so
// that a keyframe can be made of a screen that hasn't changed since.
//
// No GL and no sockets here.
#pragma once

#include "irisenc.h"
#include "pictures.h"

#include <string>
#include <vector>

namespace media {

class PanelEncoder {
public:
    // False with `err`: GBM or the encoder refused, or the encoder's layout isn't the pictures'.
    bool Open(int width, int height, int fps, int bitrate, std::string &err);
    void Close();
    bool open() const { return enc_.isOpen(); }
    // Readable when an encoded picture waits.
    int fd() const { return enc_.fd(); }
    Pictures &pictures() { return pictures_; }

    // A picture to draw into: one the encoder doesn't read that isn't the last one queued. -1
    // when there is none now.
    int Free();
    // Hands picture `i`, drawn, to the encoder, as a keyframe if `key`.
    bool Queue(int i, bool key);
    // The last picture queued, once more: false when there is none, or the encoder still reads it.
    bool Again(bool key);
    // The next encoded picture, if one waits. `u.data` stays valid until the next call.
    bool Next(irisenc::Unit &u);
    void Bitrate(int bitsPerSecond);
    const std::string &lastError() const { return enc_.lastError(); }

private:
    void TakeBack();

    irisenc::Encoder enc_;
    Pictures pictures_;
    int slot_[Pictures::kCount] = {-1, -1, -1, -1};   // the encoder slot a picture is queued in, or -1
    int last_ = -1;
    int bitrate_ = 0;
    std::vector<int> back_;
};

}  // namespace media
