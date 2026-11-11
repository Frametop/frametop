// The datagrams on ft-together's local socket that aren't text (together/README.md,
// "ft-together"): a question there is a line of text, and these are told from one by their
// first byte, which no text starts with.
//
//   to ft-together     a pose datagram as it goes on a link (proto/posewire.h: its first byte
//                      is proto::kPoseDatagram, 1): your own pose now, in the room's directions.
//                      It isn't answered.
//   to a watcher       kPoseToWatcher, whose it is (their place in the room, a byte), then that
//                      person's pose datagram (watchers.h)
#pragma once

#include <cstdint>

namespace local {

constexpr uint8_t kPoseToWatcher = 0x81;

}  // namespace local
