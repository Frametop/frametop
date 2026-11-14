// The datagrams on ft-together's local socket that aren't text (together/README.md,
// "ft-together"): a question there is a line of text, and these are told from one by their
// first byte, which no text starts with.
//
//   to ft-together     a pose datagram as it goes on a link (proto/posewire.h: its first byte
//                      is proto::kPoseDatagram, 1): your own pose now, in the room's directions.
//                      It isn't answered.
//   to a watcher       kPoseToWatcher, whose it is (their place in the room, a byte), then that
//                      person's pose datagram (watchers.h)
//   to ft-together     a voice datagram as it goes on a link (proto/voicewire.h: its first
//                      byte is proto::kVoiceDatagram, 2): 20 ms of your own voice. It isn't
//                      answered.
//   to a listener      kVoiceToListener, whose it is, then that person's voice datagram
//   to ft-together     kMouthToTogether, then the five weights of your own mouth's shapes now
//                      (proto/posewire.h's `mouth`: aa, ih, ou, ee, oh, 255 whole), from the
//                      program that hears you. It goes out in your poses (mouth.h), and isn't
//                      answered.
//   to ft-together     a pointer datagram as it goes on a link (proto/pointwire.h: its first
//                      byte is proto::kPointDatagram, 3): where you point now. It isn't answered.
//   to who asks `points`  kPointToWatcher, whose it is, then that person's pointer datagram
//   to who asks `points`  kGlanceToWatcher, whose it is, then a pointer datagram that says where
//                      on a panel that person looks, while they share it (glances.h)
//   to who asks `inputs`  kInputToTaker, whose it is, then an act's body (proto/control.h):
//                      input someone may give this Frame's desktop now (inputs.h)
#pragma once

#include "../proto/posewire.h"

#include <cstddef>
#include <cstdint>

namespace local {

constexpr uint8_t kPoseToWatcher = 0x81;
constexpr uint8_t kVoiceToListener = 0x82;
constexpr uint8_t kMouthToTogether = 0x83;
constexpr uint8_t kPointToWatcher = 0x84;
constexpr uint8_t kInputToTaker = 0x85;
constexpr uint8_t kGlanceToWatcher = 0x86;
constexpr size_t kMouthBytes = 1 + proto::kMouthShapes;

}  // namespace local
