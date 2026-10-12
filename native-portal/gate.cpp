// Launch-only independent native camera request hooks. No Vulkan exports.
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
alignas(4) uint32_t above_menu = 0, requested = 0, full_room = 0;
bool gate_ready = false;
using CameraDraw = char (*)(void *, uint32_t, void *, const void *, const void *);
CameraDraw camera_draw = nullptr;
uint32_t camera_request(uint32_t available, uint32_t native) {
  __atomic_store_n(&full_room, native ? 1u : 0u, __ATOMIC_RELAXED);
  return native || ((available & 0xffu) && __atomic_load_n(&requested, __ATOMIC_RELAXED));
}
char masked_camera_draw(void *camera, uint32_t eye, void *state,
                        const void *controls, const void *arguments) {
  if (__atomic_load_n(&full_room, __ATOMIC_RELAXED) ||
      !__atomic_load_n(&requested, __ATOMIC_RELAXED))
    return camera_draw(camera, eye, state, controls, arguments);
  alignas(16) unsigned char local_controls[kCameraControlBytes];
  alignas(16) unsigned char local_arguments[kCameraArgumentBytes];
  memcpy(local_controls, controls, sizeof local_controls);
  memcpy(local_arguments, arguments, sizeof local_arguments);
  local_controls[0] = 1; // per-call explicit camera path, not shared RoomView state
  const float opacity = 1.0f;
  memcpy(local_arguments+kCameraOpacityOffset, &opacity, sizeof opacity);
  return camera_draw(camera, eye, state, local_controls, local_arguments);
}
bool branch(uintptr_t from, uintptr_t to, uint32_t &out, bool link = false) {
  const int64_t delta = int64_t(to) - int64_t(from);
  if ((delta & 3) || delta < -(INT64_C(1) << 27) || delta >= (INT64_C(1) << 27)) return false;
  out = (link ? 0x94000000 : 0x14000000) | (uint32_t(delta / 4) & 0x03ffffff);
  return true;
}
void *trampolines(uintptr_t request, uintptr_t order, uintptr_t before, uintptr_t after,
                  uintptr_t target, size_t page_size, uint32_t entries[4]) {
  const uintptr_t origin = request & ~(uintptr_t(page_size)-1);
  for (uintptr_t distance = 0x10000; distance < 0x7000000; distance += 0x10000) {
    for (int direction : {-1, 1}) {
      if (direction < 0 && origin < distance) continue;
      uintptr_t address = direction < 0 ? origin-distance : origin+distance;
      void *memory = mmap(reinterpret_cast<void *>(address), page_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (memory == MAP_FAILED) continue;
      auto *code = static_cast<uint32_t *>(memory);
      memcpy(code, kRequestTrampoline, sizeof kRequestTrampoline);
      uintptr_t helper = reinterpret_cast<uintptr_t>(&camera_request);
      memcpy(code+10, &helper, 8); memcpy(code+12, &target, 8);
      memcpy(code+16, kOrderTrampoline, sizeof kOrderTrampoline);
      uintptr_t stock = reinterpret_cast<uintptr_t>(&full_room);
      uintptr_t above = reinterpret_cast<uintptr_t>(&above_menu);
      memcpy(code+28, &stock, 8); memcpy(code+30, &above, 8);
      code[32] = 0x58000050; // ldr x16,+8
      code[33] = 0xd61f0200; // br x16: preserve native BL return address
      uintptr_t draw = reinterpret_cast<uintptr_t>(&masked_camera_draw);
      memcpy(code+34, &draw, 8);
      if (!branch(request,address,entries[0],true) ||
          !branch(order,address+64,entries[1]) ||
          !branch(before,address+128,entries[2],true) ||
          !branch(after,address+128,entries[3],true) ||
          !branch(address+96,order+4,code[24]) || !branch(address+104,order+4,code[26])) {
        munmap(memory,page_size); continue;
      }
      __builtin___clear_cache(reinterpret_cast<char *>(memory),reinterpret_cast<char *>(memory)+144);
      if (mprotect(memory,page_size,PROT_READ|PROT_EXEC)) { munmap(memory,page_size); return nullptr; }
      return memory;
    }
  }
  return nullptr;
}
int gate(dl_phdr_info *info, size_t, void *context) {
  const auto &profile = *static_cast<const CompositorProfile *>(context);
  if (info->dlpi_name && *info->dlpi_name) return 0;
  if (!profile.camera_request) {
    publish_status(false,"independent-camera-contract-unverified",&profile);
    return 1;
  }
  const uintptr_t offsets[] = {profile.camera_request,profile.order_selector,profile.camera_before,profile.camera_after,profile.aurora_gate};
  const size_t sizes[] = {4,sizeof kOrderOriginal,4,4,4};
  uint32_t *sites[5]{};
  for (unsigned j=0; j<5; ++j) {
    bool executable = false;
    for (unsigned i=0; i<info->dlpi_phnum; ++i) {
      const auto &p = info->dlpi_phdr[i];
      if (p.p_type == PT_LOAD && (p.p_flags & PF_X) && p.p_vaddr <= offsets[j] &&
          p.p_vaddr+p.p_memsz >= offsets[j]+sizes[j]) executable = true;
    }
    if (!executable) return 1;
    sites[j] = reinterpret_cast<uint32_t *>(info->dlpi_addr+offsets[j]);
  }
  if (*sites[0] != profile.request_original || *sites[2] != profile.before_original ||
      *sites[3] != profile.after_original || *sites[4] != kAuroraOriginal ||
      memcmp(sites[1],kOrderOriginal,sizeof kOrderOriginal)) {
    fprintf(stderr,"ft-passthrough: native camera contract instruction mismatch\n"); return 1;
  }
  const long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0 || (ps & (ps-1))) return 1;
  uint32_t entries[4]{};
  void *trampoline = trampolines(uintptr_t(sites[0]),uintptr_t(sites[1]),uintptr_t(sites[2]),uintptr_t(sites[3]),
                                 info->dlpi_addr+profile.request_target,size_t(ps),entries);
  if (!trampoline) { fprintf(stderr,"ft-passthrough: native camera trampoline unavailable\n"); return 1; }
  // Prepare every touched page before writing any instruction. The Aurora gate
  // stays stock, preserving the user's environment/full-room choice.
  void *pages[4]{}; unsigned count=0;
  for (unsigned i=0;i<4;++i) {
    void *page = reinterpret_cast<void *>(uintptr_t(sites[i]) & ~(uintptr_t(ps)-1));
    bool seen=false; for (unsigned j=0;j<count;++j) seen |= pages[j]==page;
    if (!seen) pages[count++]=page;
  }
  for (unsigned i=0;i<count;++i) if (mprotect(pages[i],size_t(ps),PROT_READ|PROT_WRITE)) {
    for (unsigned j=0;j<i;++j) if (mprotect(pages[j],size_t(ps),PROT_READ|PROT_EXEC)) _exit(190);
    munmap(trampoline,size_t(ps)); return 1;
  }
  camera_draw = reinterpret_cast<CameraDraw>(info->dlpi_addr+profile.camera_target);
  for (unsigned i=0;i<4;++i) {
    __atomic_store_n(sites[i],entries[i],__ATOMIC_RELEASE);
    __builtin___clear_cache(reinterpret_cast<char *>(sites[i]),reinterpret_cast<char *>(sites[i]+1));
  }
  for (unsigned i=0;i<count;++i) if (mprotect(pages[i],size_t(ps),PROT_READ|PROT_EXEC)) _exit(190);
  gate_ready = true;
  publish_status(false,"waiting-for-device",&profile);
  fprintf(stderr,"ft-passthrough: independent native camera request and masked draw hooks installed; stock Aurora gate retained\n");
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
void ft_passthrough_set_request(uint32_t demand, uint32_t above) {
  __atomic_store_n(&above_menu, demand && above ? 1u : 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&requested, demand ? 1u : 0u, __ATOMIC_RELAXED);
}
extern "C" __attribute__((visibility("default")))
bool ft_passthrough_full_room() { return __atomic_load_n(&full_room,__ATOMIC_RELAXED) != 0; }
extern "C" __attribute__((visibility("default")))
bool ft_passthrough_gate_ready() { return gate_ready; }
