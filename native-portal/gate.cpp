// Launch-only Aurora scene gate. No Vulkan exports.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <initializer_list>
#include <link.h>
#include "compatibility.h"
#include <sys/mman.h>
#include <unistd.h>
namespace {
using namespace compatibility;
alignas(4) uint32_t above_menu = 0;
bool gate_ready = false;
bool branch(uintptr_t from, uintptr_t to, uint32_t &out) {
  const int64_t delta = int64_t(to) - int64_t(from);
  if ((delta & 3) || delta < -(INT64_C(1) << 27) ||
      delta >= (INT64_C(1) << 27)) return false;
  out = 0x14000000 | (uint32_t(delta / 4) & 0x03ffffff);
  return true;
}
// Only w0 is changed, exactly as by the displaced LDR. No BL, stack change,
// flag change, SIMD instruction, callback or resource access occurs here.
void *order_trampoline(uintptr_t site, size_t page_size, uint32_t &entry) {
  const uintptr_t origin = site & ~(uintptr_t(page_size) - 1);
  for (uintptr_t distance = 0x10000; distance < 0x7000000; distance += 0x10000) {
    for (int direction : {-1, 1}) {
      if (direction < 0 && origin < distance) continue;
      uintptr_t address = direction < 0 ? origin-distance : origin+distance;
      void *memory = mmap(reinterpret_cast<void *>(address), page_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                          -1, 0);
      if (memory == MAP_FAILED) continue;
      auto *code = static_cast<uint32_t *>(memory);
      // Native portal-stencil mode keeps its original selector. Ordinary
      // RoomView selects its existing pre/post-overlay camera paths instead.
      memcpy(code, kOrderTrampoline, sizeof(kOrderTrampoline));
      const uintptr_t pointer = reinterpret_cast<uintptr_t>(&above_menu);
      memcpy(code+8, &pointer, sizeof pointer);
      if (!branch(site, address, entry) ||
          !branch(address+20, site+4, code[5]) ||
          !branch(address+28, site+4, code[7])) {
        munmap(memory,page_size); continue;
      }
      __builtin___clear_cache(reinterpret_cast<char *>(memory),
                              reinterpret_cast<char *>(memory)+40);
      if (mprotect(memory,page_size,PROT_READ|PROT_EXEC)) {
        munmap(memory,page_size); return nullptr;
      }
      return memory;
    }
  }
  return nullptr;
}

int gate(dl_phdr_info *info, size_t, void *context) {
  const auto &profile = *static_cast<const CompositorProfile *>(context);
  if (info->dlpi_name && *info->dlpi_name)
    return 0;
  auto *word = reinterpret_cast<uint32_t *>(info->dlpi_addr + profile.aurora_gate);
  auto *order = reinterpret_cast<uint32_t *>(info->dlpi_addr + profile.order_selector);
  bool executable = false;
  for (unsigned i = 0; i < info->dlpi_phnum; i++) {
    const auto &p = info->dlpi_phdr[i];
    if (p.p_type == PT_LOAD && (p.p_flags & PF_X) && p.p_vaddr <= profile.aurora_gate &&
        p.p_vaddr + p.p_memsz >= profile.order_selector + sizeof(kOrderOriginal))
      executable = true;
  }
  if (!executable || *word != kAuroraOriginal ||
      memcmp(order, kOrderOriginal, sizeof(kOrderOriginal))) {
    fprintf(stderr, "ft-passthrough: Aurora gate original mismatch\n");
    return 1;
  }
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0 || (ps & (ps-1)))
    return 1;
  auto *page = reinterpret_cast<void *>(uintptr_t(word) & ~(uintptr_t(ps) - 1));
  if ((uintptr_t(order) & ~(uintptr_t(ps)-1)) != uintptr_t(page)) return 1;
  uint32_t order_branch = 0;
  void *trampoline = order_trampoline(uintptr_t(order), size_t(ps), order_branch);
  if (!trampoline) {
    fprintf(stderr,"ft-passthrough: camera order trampoline unavailable\n");
    return 1;
  }
  if (mprotect(page, size_t(ps), PROT_READ | PROT_WRITE)) {
    munmap(trampoline,size_t(ps));
    perror("ft-passthrough gate mprotect");
    return 1;
  }
  __atomic_store_n(word, kNop, __ATOMIC_RELEASE);
  __atomic_store_n(order, order_branch, __ATOMIC_RELEASE);
  __builtin___clear_cache(reinterpret_cast<char *>(word),
                          reinterpret_cast<char *>(order + 1));
  if (mprotect(page, size_t(ps), PROT_READ | PROT_EXEC))
    _exit(190);
  gate_ready = true;
  publish_status(false, "waiting-for-device", &profile);
  fprintf(stderr, "ft-passthrough: Aurora gate patched\n");
  fprintf(stderr,"ft-passthrough: native camera pre/post-overlay selector installed\n");
  return 1;
}
__attribute__((constructor)) void launch_gate() {
  const char *value = getenv("FT_PASSTHROUGH_ENABLE");
  if (!value || strcmp(value, "1"))
    return;
  // LD_PRELOAD is inherited by launch helpers. Ignore those before hashing.
  char executable[4096];
  ssize_t length =
      readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length <= 0 || size_t(length) >= sizeof(executable) - 1)
    return;
  executable[length] = 0;
  const char *base = strrchr(executable, '/');
  if (!base || strcmp(base + 1, kCompositorName))
    return;
  publish_status(false, "gate-not-ready");
  DIR *dir = opendir("/proc/self/task");
  if (!dir)
    return;
  unsigned threads = 0;
  while (auto *entry = readdir(dir))
    if (entry->d_name[0] != '.')
      threads++;
  closedir(dir);
  if (threads != 1) {
    publish_status(false, "gate-requires-single-thread");
    fprintf(stderr, "ft-passthrough: Aurora gate requires single-threaded preload\n");
    return;
  }
  const auto *profile = executable_profile();
  if (!profile) {
    publish_status(false, "unsupported-executable");
    fprintf(stderr, "ft-passthrough: Aurora gate executable pin rejected\n");
    return;
  }
  dl_iterate_phdr(gate, const_cast<CompositorProfile *>(profile));
}
} // namespace
extern "C" __attribute__((visibility("default")))
void ft_passthrough_set_above_menu(uint32_t enabled) {
  __atomic_store_n(&above_menu, enabled ? 1u : 0u, __ATOMIC_RELAXED);
}

extern "C" __attribute__((visibility("default")))
bool ft_passthrough_gate_ready() { return gate_ready; }
