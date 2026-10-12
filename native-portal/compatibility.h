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
};
// Full-image identity selects addresses; instruction and stack contracts below
// are shared only because both versions were independently inspected.
inline constexpr CompositorProfile kCompositors[] = {
    {"9b1bd5fe4463592cbc71b2517b9ceb0a0fcd66cd6885a19dca4be0784bf6c265", 0x9f404, 0x9fab0}, // SteamOS 0.3.0
    {"0bb148eda6b5e54e2fbed09d475a43671f5ff6fcb6c2b4f48345b1a9fe8d6f96", 0xa246c, 0xa2b18}, // SteamOS 0.4.5
};
inline constexpr char kCompositorName[] = "vrcompositor";
inline constexpr char kVertexEntrypoint[] = "VS_MAIN";
inline constexpr char kFragmentEntrypoint[] = "PS_MAIN";
// ELF virtual addresses, added to the main executable's load bias.
inline constexpr uint32_t kAuroraOriginal = 0x34003577; // cbz w23, ...
inline constexpr uint32_t kNop = 0xd503201f;
inline constexpr uint32_t kOrderOriginal[] = {
    0xb9410be0, // ldr w0,[sp,#0x108]: existing before/after-overlay selector
    0x7100001f, // cmp w0,#0
    0xb940d3e0, // ldr w0,[sp,#0xd0]: native stencil mode
};
// AArch64 trampoline. Words 5/7 are filled with checked B instructions; the
// selector pointer follows at byte 32. Only x0 is clobbered; flags stay intact.
inline constexpr uint32_t kOrderTrampoline[] = {
    kOrderOriginal[2], // ldr w0,[sp,#0xd0]
    0x350000a0,        // cbnz w0,+20: keep native stencil selector
    0x580000c0,        // ldr x0,+24: selector pointer at byte 32
    0xb9400000,        // ldr w0,[x0]
    0xb9010be0,        // str w0,[sp,#0x108]
    0,                // b original selector + 4
    kOrderOriginal[0], // displaced ldr w0,[sp,#0x108]
    0,                // b original selector + 4
};

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
