// Compatibility contract for the tested native compositor. See COMPATIBILITY.md.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/evp.h>

namespace compatibility {
struct CompositorProfile {
  const char *sha256;
  uintptr_t aurora_gate;
  uintptr_t order_selector;
  uintptr_t camera_request;
  uintptr_t request_target;
  uintptr_t camera_before;
  uintptr_t camera_after;
  uintptr_t camera_target;
  uint32_t request_original;
  uint32_t before_original;
  uint32_t after_original;
};
// Both independent camera-request/call contracts were checked against retained
// REA evidence and original bytes. Independent-mode live tests used 0.4.5.
inline constexpr CompositorProfile kCompositors[] = {
    {"9b1bd5fe4463592cbc71b2517b9ceb0a0fcd66cd6885a19dca4be0784bf6c265", 0x9f404, 0x9fab0,
     0x9ee6c, 0x26a378, 0xa0dbc, 0xa03f8, 0x160be8, 0x94072d43, 0x9402ff8b, 0x940301fc},
    {"0bb148eda6b5e54e2fbed09d475a43671f5ff6fcb6c2b4f48345b1a9fe8d6f96", 0xa246c, 0xa2b18,
     0xa1ecc, 0x271570, 0xa3e30, 0xa3464, 0x164e18, 0x94073da9, 0x940303fa, 0x9403066d},
};
inline constexpr char kCompositorName[] = "vrcompositor";
inline constexpr char kVertexEntrypoint[] = "VS_MAIN";
inline constexpr char kFragmentEntrypoint[] = "PS_MAIN";
inline constexpr uint32_t kAuroraOriginal = 0x34003577; // retained, never patched
inline constexpr uint32_t kOrderOriginal[] = {
    0xb9410be0, // ldr w0,[sp,#0x108]: existing before/after-overlay selector
    0x7100001f, // cmp w0,#0
    0xb940d3e0, // ldr w0,[sp,#0xd0]: native stencil mode
};
// Called in place of the original BL. x0 is camera availability; w27 is the
// completed native demand predicate. The helper returns combined demand in w0;
// the original singleton getter then supplies its original x0 return value.
// x27 is deliberately updated. Other callee-saved registers follow AAPCS64.
inline constexpr uint32_t kRequestTrampoline[] = {
    0xa9bf7bfd, // stp x29,x30,[sp,#-16]!
    0x2a1b03e1, // mov w1,w27
    0x58000110, // ldr x16,+32: helper pointer at byte40
    0xd63f0200, // blr x16
    0x2a0003fb, // mov w27,w0
    0x580000f0, // ldr x16,+28: original target pointer at byte48
    0xd63f0200, // blr x16
    0xa8c17bfd, // ldp x29,x30,[sp],#16
    0xd65f03c0, // ret to original call site+4
    0xd503201f,
};
// No stack or flag changes. Stock camera demand and native stencil retain their
// selector. Literal pointers to full_room/above_menu follow at byte48/56.
inline constexpr uint32_t kOrderTrampoline[] = {
    0x58000180, 0xb9400000, 0x350000e0,
    kOrderOriginal[2], 0x350000a0,
    0x58000120, 0xb9400000, 0xb9010be0,
    0, kOrderOriginal[0], 0, 0xd503201f,
};
// Only these argument bytes are read by the pinned camera renderer. Mask-only
// wrappers copy them on stack; all original matrices and resource handles stay.
inline constexpr size_t kCameraControlBytes = 0x18;
inline constexpr size_t kCameraArgumentBytes = 0x90;
inline constexpr size_t kCameraOpacityOffset = 0x0c;

inline const CompositorProfile *executable_profile() {
  FILE *f = fopen("/proc/self/exe", "rb");
  if (!f)
    return nullptr;
  auto *ctx = EVP_MD_CTX_new();
  bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
  unsigned char buffer[65536], digest[32];
  size_t n;
  unsigned size = 0;
  while (ok && (n = fread(buffer, 1, sizeof buffer, f)))
    ok = EVP_DigestUpdate(ctx, buffer, n) == 1;
  ok = ok && !ferror(f) && EVP_DigestFinal_ex(ctx, digest, &size) == 1 &&
       size == 32;
  fclose(f);
  EVP_MD_CTX_free(ctx);
  char hex[65]{};
  if (ok)
    for (unsigned i = 0; i < 32; i++)
      snprintf(hex + i * 2, 3, "%02x", digest[i]);
  if (ok) for (const auto &profile : kCompositors)
    if (!strcmp(hex, profile.sha256)) return &profile;
  return nullptr;
}
// Initialization evidence, not a promise that camera pixels have been rendered.
// A reader must match PID/start time and /proc/PID/exe identity before trusting it.
inline bool compositor_process() {
  char path[4096];
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path)-1);
  if (n <= 0 || size_t(n) >= sizeof(path)-1) return false;
  path[n] = 0;
  const char *base = strrchr(path, '/');
  return base && !strcmp(base+1, kCompositorName);
}
inline void publish_status(bool ready, const char *reason,
                           const CompositorProfile *profile = nullptr) {
  if (!compositor_process()) return; // inherited layer/preload in launch helpers
  const char *configured = getenv("FT_PASSTHROUGH_STATUS");
  const char *mask = getenv("FT_PASSTHROUGH_MASK");
  std::string path = configured ? configured : mask ? std::string(mask)+".status" : "";
  if (path.empty() || path[0] != '/') return;
  struct stat target{}, executable{};
  // Never replace a symlink, special file, or another user's status file.
  if (lstat(path.c_str(), &target) == 0 &&
      (!S_ISREG(target.st_mode) || target.st_uid != getuid())) return;
  if (stat("/proc/self/exe", &executable)) return;
  FILE *proc = fopen("/proc/self/stat", "r");
  if (!proc) return;
  char line[4096];
  const bool read = fgets(line, sizeof line, proc) != nullptr;
  fclose(proc);
  char *tail = read ? strrchr(line, ')') : nullptr;
  if (!tail) return;
  char *save = nullptr, *field = strtok_r(tail+1, " ", &save);
  for (int number = 3; field && number < 22; ++number) field = strtok_r(nullptr, " ", &save);
  if (!field) return;
  const unsigned long long start = strtoull(field, nullptr, 10);
  if (!start) return;
  std::string temporary = path + ".tmp.XXXXXX";
  int fd = mkstemp(temporary.data()); // private 0600, never follows an existing link
  if (fd < 0) return;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  FILE *out = fdopen(fd, "w");
  if (!out) { close(fd); unlink(temporary.c_str()); return; }
  const int written = fprintf(out,
      "{\"version\":1,\"pid\":%ld,\"start_ticks\":%llu,\"exe_dev\":%llu,"
      "\"exe_ino\":%llu,\"sha256\":\"%s\",\"ready\":%s,\"reason\":\"%s\"}\n",
      long(getpid()), start, static_cast<unsigned long long>(executable.st_dev),
      static_cast<unsigned long long>(executable.st_ino), profile ? profile->sha256 : "",
      ready ? "true" : "false", reason);
  const bool closed = fclose(out) == 0;
  if (written > 0 && closed) rename(temporary.c_str(), path.c_str());
  unlink(temporary.c_str());
}
inline bool executable_matches() { return executable_profile() != nullptr; }
} // namespace compatibility
