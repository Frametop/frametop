// Explicit passthrough layer. Native commands and resource lifetimes are untouched.
// The optional Aurora patch is a separate launch-only gate DSO.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include "compatibility.h"
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vk_layer.h>

#include "pinned.hpp"
#define EXPORT extern "C" __attribute__((visibility("default")))
namespace {
struct InstanceDispatch {
  VkInstance instance;
  PFN_vkGetInstanceProcAddr gipa;
  PFN_GetPhysicalDeviceProcAddr gpdpa;
  PFN_vkDestroyInstance destroy;
};
struct DeviceDispatch {
  PFN_vkGetDeviceProcAddr gdpa;
  PFN_vkCreateShaderModule module;
  PFN_vkDestroyDevice destroy;
  PFN_vkDestroyShaderModule destroy_module;
  PFN_vkCreateGraphicsPipelines pipelines;
  PFN_vkCreatePipelineLayout create_layout;
  PFN_vkDestroyPipelineLayout destroy_layout;
  PFN_vkDestroyPipeline destroy_pipeline;
  PFN_vkCmdBindPipeline bind_pipeline;
  PFN_vkCmdPushConstants push;
  PFN_vkCmdDrawIndexed draw_indexed;
  PFN_vkAllocateCommandBuffers allocate_commands;
  PFN_vkFreeCommandBuffers free_commands;
  PFN_vkBeginCommandBuffer begin_command;
  PFN_vkResetCommandBuffer reset_command;
  PFN_vkResetCommandPool reset_pool;
  PFN_vkDestroyCommandPool destroy_pool;
  VkDevice device{};
  VkDescriptorSetLayout hand_layout{};
  VkPhysicalDeviceMemoryProperties memory{};
};
std::mutex dispatch_mutex;
std::unordered_map<void *, InstanceDispatch> instances;
std::unordered_map<void *, DeviceDispatch> devices;
std::unordered_map<void *, std::unordered_map<VkShaderModule, int>> modules;
struct CommandState {
  void *device{};
  VkCommandPool pool{};
  VkPipelineLayout layout{};
  bool sampled{};
  std::array<float, 528> geometry{};
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  void *mapped{};
  VkDescriptorPool descriptors{};
  VkDescriptorSet hands{};
};
std::unordered_map<void *, std::unordered_set<VkPipelineLayout>> layouts;
std::unordered_map<void *, std::unordered_map<VkPipeline, VkPipelineLayout>>
    camera_pipelines;
std::unordered_map<VkCommandBuffer, CommandState> commands;
unsigned draw_logs = 0;
void *key(const void *handle) {
  return handle ? *static_cast<void *const *>(handle) : nullptr;
}
InstanceDispatch instance_dispatch(VkInstance i) {
  std::lock_guard<std::mutex> l(dispatch_mutex);
  auto p = instances.find(key(i));
  return p == instances.end() ? InstanceDispatch{} : p->second;
}
DeviceDispatch device_dispatch(VkDevice d) {
  std::lock_guard<std::mutex> l(dispatch_mutex);
  auto p = devices.find(key(d));
  return p == devices.end() ? DeviceDispatch{} : p->second;
}
#define FN(d, name) reinterpret_cast<PFN_##name>((d).gdpa((d).device, #name))
void release_hands(const DeviceDispatch &d, CommandState &s) {
  if (s.descriptors) FN(d, vkDestroyDescriptorPool)(d.device, s.descriptors, nullptr);
  if (s.mapped) FN(d, vkUnmapMemory)(d.device, s.memory);
  if (s.buffer) FN(d, vkDestroyBuffer)(d.device, s.buffer, nullptr);
  if (s.memory) FN(d, vkFreeMemory)(d.device, s.memory, nullptr);
  s.descriptors = VK_NULL_HANDLE; s.hands = VK_NULL_HANDLE;
  s.buffer = VK_NULL_HANDLE; s.memory = VK_NULL_HANDLE; s.mapped = nullptr;
}
bool prepare_hands(const DeviceDispatch &d, CommandState &s) {
  // One immutable snapshot per native command recording. Native begin/reset may
  // only happen once previous execution has completed, so mapped reuse is safe.
  if (!s.buffer) {
    VkBufferCreateInfo buffer{}; buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = 2048; buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (FN(d, vkCreateBuffer)(d.device, &buffer, nullptr, &s.buffer) != VK_SUCCESS) goto fail;
    VkMemoryRequirements req;
    FN(d, vkGetBufferMemoryRequirements)(d.device, s.buffer, &req);
    uint32_t type = 0;
    for (; type < d.memory.memoryTypeCount; ++type)
      if ((req.memoryTypeBits & (1u << type)) &&
          (d.memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    if (type == d.memory.memoryTypeCount) goto fail;
    VkMemoryAllocateInfo allocation{}; allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = req.size; allocation.memoryTypeIndex = type;
    if (FN(d, vkAllocateMemory)(d.device, &allocation, nullptr, &s.memory) != VK_SUCCESS ||
        FN(d, vkBindBufferMemory)(d.device, s.buffer, s.memory, 0) != VK_SUCCESS ||
        FN(d, vkMapMemory)(d.device, s.memory, 0, VK_WHOLE_SIZE, 0, &s.mapped) != VK_SUCCESS) goto fail;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
    if (FN(d, vkCreateDescriptorPool)(d.device, &pool, nullptr, &s.descriptors) != VK_SUCCESS) goto fail;
    VkDescriptorSetAllocateInfo sets{}; sets.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sets.descriptorPool = s.descriptors; sets.descriptorSetCount = 1; sets.pSetLayouts = &d.hand_layout;
    if (FN(d, vkAllocateDescriptorSets)(d.device, &sets, &s.hands) != VK_SUCCESS) goto fail;
    VkDescriptorBufferInfo info{s.buffer, 0, 2048};
    VkWriteDescriptorSet write{}; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = s.hands; write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo = &info;
    FN(d, vkUpdateDescriptorSets)(d.device, 1, &write, 0, nullptr);
  }
  memcpy(s.mapped, s.geometry.data()+16, 2048);
  return true;
fail:
  release_hands(d, s);
  fprintf(stderr, "ft-passthrough: hand mask allocation failed; skipping camera draw\n");
  return false;
}

std::once_flag config_once;
bool enabled = false;

void load_configuration() {
  enabled = compatibility::executable_matches();
  fprintf(stderr, "ft-passthrough: executable pin %s\n",
          enabled ? "accepted" : "rejected");
}
VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *ci,
                                    const VkAllocationCallbacks *a,
                                    VkInstance *out) {
  std::call_once(config_once, load_configuration);
  auto chain = (VkLayerInstanceCreateInfo *)ci->pNext;
  while (chain &&
         (chain->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
          chain->function != VK_LAYER_LINK_INFO))
    chain = (VkLayerInstanceCreateInfo *)chain->pNext;
  if (!chain || !chain->u.pLayerInfo)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto link = chain->u.pLayerInfo;
  auto next = link->pfnNextGetInstanceProcAddr;
  auto physical = link->pfnNextGetPhysicalDeviceProcAddr;
  auto create = (PFN_vkCreateInstance)next(nullptr, "vkCreateInstance");
  if (!create)
    return VK_ERROR_INITIALIZATION_FAILED;
  chain->u.pLayerInfo = link->pNext;
  VkResult r = create(ci, a, out);
  if (r != VK_SUCCESS)
    return r;
  InstanceDispatch dispatch{
      *out, next, physical,
      (PFN_vkDestroyInstance)next(*out, "vkDestroyInstance")};
  try {
    std::lock_guard<std::mutex> l(dispatch_mutex);
    instances.emplace(key(*out), dispatch);
  } catch (...) {
    dispatch.destroy(*out, a);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  return r;
}
void VKAPI_CALL destroy_instance(VkInstance i, const VkAllocationCallbacks *a) {
  auto d = instance_dispatch(i);
  if (!d.destroy)
    return;
  {
    std::lock_guard<std::mutex> l(dispatch_mutex);
    instances.erase(key(i));
  }
  d.destroy(i, a);
}
VkResult VKAPI_CALL create_device(VkPhysicalDevice p,
                                  const VkDeviceCreateInfo *ci,
                                  const VkAllocationCallbacks *a,
                                  VkDevice *out) {
  auto parent = instance_dispatch((VkInstance)p);
  if (!parent.gipa)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto chain = (VkLayerDeviceCreateInfo *)ci->pNext;
  while (chain &&
         (chain->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
          chain->function != VK_LAYER_LINK_INFO))
    chain = (VkLayerDeviceCreateInfo *)chain->pNext;
  if (!chain || !chain->u.pLayerInfo)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto link = chain->u.pLayerInfo;
  auto gdpa = link->pfnNextGetDeviceProcAddr;
  auto create = (PFN_vkCreateDevice)link->pfnNextGetInstanceProcAddr(
      parent.instance, "vkCreateDevice");
  if (!create || !gdpa)
    return VK_ERROR_INITIALIZATION_FAILED;
  chain->u.pLayerInfo = link->pNext;
  VkResult r = create(p, ci, a, out);
  if (r != VK_SUCCESS)
    return r;
  DeviceDispatch d{
      gdpa,
      (PFN_vkCreateShaderModule)gdpa(*out, "vkCreateShaderModule"),
      (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice"),
      (PFN_vkDestroyShaderModule)gdpa(*out, "vkDestroyShaderModule"),
      (PFN_vkCreateGraphicsPipelines)gdpa(*out, "vkCreateGraphicsPipelines"),
      (PFN_vkCreatePipelineLayout)gdpa(*out, "vkCreatePipelineLayout"),
      (PFN_vkDestroyPipelineLayout)gdpa(*out, "vkDestroyPipelineLayout"),
      (PFN_vkDestroyPipeline)gdpa(*out, "vkDestroyPipeline"),
      (PFN_vkCmdBindPipeline)gdpa(*out, "vkCmdBindPipeline"),
      (PFN_vkCmdPushConstants)gdpa(*out, "vkCmdPushConstants"),
      (PFN_vkCmdDrawIndexed)gdpa(*out, "vkCmdDrawIndexed"),
      (PFN_vkAllocateCommandBuffers)gdpa(*out, "vkAllocateCommandBuffers"),
      (PFN_vkFreeCommandBuffers)gdpa(*out, "vkFreeCommandBuffers"),
      (PFN_vkBeginCommandBuffer)gdpa(*out, "vkBeginCommandBuffer"),
      (PFN_vkResetCommandBuffer)gdpa(*out, "vkResetCommandBuffer"),
      (PFN_vkResetCommandPool)gdpa(*out, "vkResetCommandPool"),
      (PFN_vkDestroyCommandPool)gdpa(*out, "vkDestroyCommandPool")};
  d.device = *out;
  reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(parent.gipa(parent.instance, "vkGetPhysicalDeviceMemoryProperties"))(p, &d.memory);
  if (enabled) {
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1; info.pBindings = &binding;
    r = FN(d, vkCreateDescriptorSetLayout)(*out, &info, nullptr, &d.hand_layout);
    if (r != VK_SUCCESS) { d.destroy(*out, a); return r; }
  }
  try {
    std::lock_guard<std::mutex> l(dispatch_mutex);
    devices.emplace(key(*out), d);
  } catch (...) {
    if (d.hand_layout) FN(d, vkDestroyDescriptorSetLayout)(*out, d.hand_layout, nullptr);
    d.destroy(*out, a);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  return r;
}
void VKAPI_CALL destroy_device(VkDevice device,
                               const VkAllocationCallbacks *a) {
  auto d = device_dispatch(device);
  if (!d.destroy)
    return;
  {
    std::lock_guard<std::mutex> l(dispatch_mutex);
    devices.erase(key(device));
    modules.erase(key(device));
    layouts.erase(key(device));
    camera_pipelines.erase(key(device));
    for (auto it = commands.begin(); it != commands.end();)
      if (it->second.device == key(device)) {
        release_hands(d, it->second);
        it = commands.erase(it);
      } else
        ++it;
  }
  if (d.hand_layout) FN(d, vkDestroyDescriptorSetLayout)(device, d.hand_layout, nullptr);
  d.destroy(device, a);
}
VkResult VKAPI_CALL create_module(VkDevice device,
                                  const VkShaderModuleCreateInfo *ci,
                                  const VkAllocationCallbacks *a,
                                  VkShaderModule *out) {
  auto d = device_dispatch(device);
  if (!d.module)
    return VK_ERROR_INITIALIZATION_FAILED;
  std::call_once(config_once, load_configuration);
  if (enabled && ci && ci->pCode)
    for (const auto &p : pins) {
      if (ci->codeSize == p.size && !memcmp(ci->pCode, p.data, p.size)) {
        VkShaderModuleCreateInfo replacement = *ci;
        replacement.codeSize = p.replacement_size;
        replacement.pCode = p.replacement;

        VkResult result = d.module(device, &replacement, a, out);
        if (result == VK_SUCCESS) {
          try {
            std::lock_guard<std::mutex> l(dispatch_mutex);
            modules[key(device)][*out] = p.id;
          } catch (...) {
            d.destroy_module(device, *out, a);
            *out = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
          }
        }
        fprintf(stderr, "ft-passthrough: shader %d result %d\n", p.id, int(result));
        return result;
      }
    }
  return d.module(device, ci, a, out);
}

void VKAPI_CALL destroy_module(VkDevice device, VkShaderModule module,
                               const VkAllocationCallbacks *a) {
  auto d = device_dispatch(device);
  if (!d.destroy_module)
    return;
  {
    std::lock_guard<std::mutex> l(dispatch_mutex);
    auto it = modules.find(key(device));
    if (it != modules.end())
      it->second.erase(module);
  }
  d.destroy_module(device, module, a);
}
VkResult VKAPI_CALL create_pipelines(VkDevice device, VkPipelineCache cache,
                                     uint32_t count,
                                     const VkGraphicsPipelineCreateInfo *ci,
                                     const VkAllocationCallbacks *a,
                                     VkPipeline *out) {
  auto d = device_dispatch(device);
  if (!d.pipelines)
    return VK_ERROR_INITIALIZATION_FAILED;
  try {
    std::vector<VkGraphicsPipelineCreateInfo> copies(ci, ci + count);
    std::vector<VkPipelineColorBlendStateCreateInfo> blends(count);
    std::vector<VkPipelineColorBlendAttachmentState> attachments(count);
    unsigned changed = 0;
    std::vector<bool> camera(count, false);
    {
      std::lock_guard<std::mutex> l(dispatch_mutex);
      auto known = modules.find(key(device));
      if (known != modules.end())
        for (uint32_t i = 0; i < count; i++) {
          int vs = 0, ps = 0;
          const auto &c = ci[i];
          if (c.stageCount != 2 || !c.pStages)
            continue;
          for (uint32_t j = 0; j < c.stageCount; j++) {
            const auto &stage = c.pStages[j];
            auto m = known->second.find(stage.module);
            if (m == known->second.end() || !stage.pName)
              continue;
            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT &&
                !strcmp(stage.pName, compatibility::kVertexEntrypoint))
              vs = m->second;
            if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT &&
                !strcmp(stage.pName, compatibility::kFragmentEntrypoint))
              ps = m->second;
          }
          bool pinned_pair = false;
          for (const auto &pair : camera_pairs)
            pinned_pair |= vs == pair[0] && ps == pair[1];
          if (!pinned_pair) continue;
          auto compatible = layouts.find(key(device));
          if (compatible == layouts.end() ||
              !compatible->second.count(c.layout)) {
            fprintf(stderr,
                    "ft-passthrough: camera pipeline has unsupported push layout\n");
            return VK_ERROR_INITIALIZATION_FAILED;
          }
          camera[i] = true;
          const auto *b = c.pColorBlendState;
          if (!b || b->attachmentCount != 1 || !b->pAttachments ||
              b->logicOpEnable)
            continue;
          const auto &original = b->pAttachments[0];
          if (!original.blendEnable ||
              original.colorBlendOp != VK_BLEND_OP_ADD ||
              original.alphaBlendOp != VK_BLEND_OP_ADD)
            continue;
          blends[i] = *b;
          attachments[i] = b->pAttachments[0];
          auto &v = attachments[i];
          v.srcColorBlendFactor = v.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
          v.dstColorBlendFactor = v.dstAlphaBlendFactor =
              VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          blends[i].pAttachments = &v;
          copies[i].pColorBlendState = &blends[i];
          changed++;
        }
    }
    VkResult result =
        d.pipelines(device, cache, count, changed ? copies.data() : ci, a, out);
    {
      std::lock_guard<std::mutex> lock(dispatch_mutex);
      for (uint32_t i = 0; i < count; i++)
        if (camera[i] && out[i] != VK_NULL_HANDLE)
          camera_pipelines[key(device)][out[i]] = ci[i].layout;
    }
    if (changed)
      fprintf(stderr, "ft-passthrough: source-over pipelines %u result %d\n", changed,
              int(result));
    return result;
  } catch (...) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
}

std::array<float, 528> read_geometry() {
  std::array<float, 528> result{};
  const char *path = getenv("FT_PASSTHROUGH_MASK");
  if (!path || path[0] != '/')
    return result;
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return result;
  struct stat st{};
  timespec now{};
  bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size == static_cast<off_t>(sizeof(result)) &&
            st.st_uid == getuid() && !clock_gettime(CLOCK_REALTIME, &now);
  double age = double(now.tv_sec - st.st_mtim.tv_sec) +
               double(now.tv_nsec - st.st_mtim.tv_nsec) * 1e-9;
  ok = ok && age >= -0.1 && age <= 0.5 && read(fd, result.data(), sizeof(result)) == static_cast<ssize_t>(sizeof(result));
  close(fd);
  for (float value : result)
    ok = ok && std::isfinite(value);
  ok = ok && result[14] >= 0 && result[14] <= 64 && std::floor(result[14]) == result[14];
  if (ok) for (unsigned i = 0; i < unsigned(result[14]); ++i)
    ok = ok && result[16+8*i+3] > 0 && result[16+8*i+3] < 1 &&
         result[16+8*i+7] > 0 && result[16+8*i+7] < 1;
  if (!ok)
    result.fill(0);
  result[12] = result[12] > 0.5f ? 1.0f : 0.0f;
  return result;
}
void update_native_order(const std::array<float,528> &geometry) {
  using SetOrder = void (*)(uint32_t);
  static const auto set_order = reinterpret_cast<SetOrder>(
      dlsym(RTLD_DEFAULT,"ft_passthrough_set_above_menu"));
  if (set_order) set_order((geometry[12] > .5f || geometry[14] > 0) && geometry[13] > .5f);
}
VkResult VKAPI_CALL create_layout(VkDevice device,
                                  const VkPipelineLayoutCreateInfo *ci,
                                  const VkAllocationCallbacks *a,
                                  VkPipelineLayout *out) {
  auto d = device_dispatch(device);
  if (!d.create_layout)
    return VK_ERROR_INITIALIZATION_FAILED;
  bool compatible = enabled && ci->pushConstantRangeCount == 0;
  VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
  auto copy = *ci;
  VkDescriptorSetLayout sets[2]{};
  if (compatible) {
    copy.pushConstantRangeCount = 1;
    copy.pPushConstantRanges = &range;
    if (ci->setLayoutCount == 1) {
      sets[0] = ci->pSetLayouts[0]; sets[1] = d.hand_layout;
      copy.setLayoutCount = 2; copy.pSetLayouts = sets;
    }
  }
  VkResult result = d.create_layout(device, compatible ? &copy : ci, a, out);
  if (enabled)
    fprintf(stderr, "ft-passthrough: layout original_ranges=%u added64=%d result=%d\n",
            ci->pushConstantRangeCount, int(compatible), int(result));
  if (result == VK_SUCCESS && compatible && ci->setLayoutCount == 1) {
    try {
      std::lock_guard<std::mutex> lock(dispatch_mutex);
      layouts[key(device)].insert(*out);
    } catch (...) {
      d.destroy_layout(device, *out, a);
      *out = VK_NULL_HANDLE;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  return result;
}
void VKAPI_CALL destroy_layout(VkDevice device, VkPipelineLayout layout,
                               const VkAllocationCallbacks *a) {
  auto d = device_dispatch(device);
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    layouts[key(device)].erase(layout);
    auto &known = camera_pipelines[key(device)];
    for (auto it = known.begin(); it != known.end();)
      if (it->second == layout) {
        fprintf(stderr,
                "ft-passthrough: camera layout destroyed while pipeline retained\n");
        it = known.erase(it);
      } else
        ++it;
    for (auto &item : commands)
      if (item.second.device == key(device) && item.second.layout == layout)
        item.second.layout = VK_NULL_HANDLE;
  }
  d.destroy_layout(device, layout, a);
}
void VKAPI_CALL destroy_pipeline(VkDevice device, VkPipeline pipeline,
                                 const VkAllocationCallbacks *a) {
  auto d = device_dispatch(device);
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    camera_pipelines[key(device)].erase(pipeline);
  }
  d.destroy_pipeline(device, pipeline, a);
}
VkResult VKAPI_CALL allocate_commands(VkDevice device,
                                      const VkCommandBufferAllocateInfo *ci,
                                      VkCommandBuffer *out) {
  auto d = device_dispatch(device);
  VkResult result = d.allocate_commands(device, ci, out);
  if (result == VK_SUCCESS) {
    try {
      std::lock_guard<std::mutex> lock(dispatch_mutex);
      for (uint32_t i = 0; i < ci->commandBufferCount; i++)
        commands[out[i]] = {key(device), ci->commandPool};
    } catch (...) {
      d.free_commands(device, ci->commandPool, ci->commandBufferCount, out);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  return result;
}
void VKAPI_CALL free_commands(VkDevice device, VkCommandPool pool,
                              uint32_t count, const VkCommandBuffer *buffers) {
  auto d = device_dispatch(device);
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    for (uint32_t i = 0; i < count; i++) {
      auto it = commands.find(buffers[i]);
      if (it != commands.end()) release_hands(d, it->second);
      commands.erase(buffers[i]);
    }
  }
  d.free_commands(device, pool, count, buffers);
}
void clear_command(CommandState &state) {
  state.layout = VK_NULL_HANDLE;
  state.sampled = false;
  state.geometry.fill(0);
}
VkResult VKAPI_CALL begin_command(VkCommandBuffer command,
                                  const VkCommandBufferBeginInfo *ci) {
  auto d = device_dispatch(reinterpret_cast<VkDevice>(command));
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    auto it = commands.find(command);
    if (it != commands.end())
      clear_command(it->second);
  }
  return d.begin_command(command, ci);
}
VkResult VKAPI_CALL reset_command(VkCommandBuffer command,
                                  VkCommandBufferResetFlags flags) {
  auto d = device_dispatch(reinterpret_cast<VkDevice>(command));
  VkResult result = d.reset_command(command, flags);
  if (result == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    auto it = commands.find(command);
    if (it != commands.end())
      clear_command(it->second);
  }
  return result;
}
VkResult VKAPI_CALL reset_pool(VkDevice device, VkCommandPool pool,
                               VkCommandPoolResetFlags flags) {
  auto d = device_dispatch(device);
  VkResult result = d.reset_pool(device, pool, flags);
  if (result == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    for (auto &item : commands)
      if (item.second.device == key(device) && item.second.pool == pool)
        clear_command(item.second);
  }
  return result;
}
void VKAPI_CALL destroy_pool(VkDevice device, VkCommandPool pool,
                             const VkAllocationCallbacks *a) {
  auto d = device_dispatch(device);
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    for (auto it = commands.begin(); it != commands.end();)
      if (it->second.device == key(device) && it->second.pool == pool) {
        release_hands(d, it->second);
        it = commands.erase(it);
      } else
        ++it;
  }
  d.destroy_pool(device, pool, a);
}
void VKAPI_CALL bind_pipeline(VkCommandBuffer command,
                              VkPipelineBindPoint point, VkPipeline pipeline) {
  auto d = device_dispatch(reinterpret_cast<VkDevice>(command));
  if (point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    auto state = commands.find(command);
    if (state != commands.end()) {
      state->second.layout = VK_NULL_HANDLE;
      auto known = camera_pipelines.find(key(command));
      if (known != camera_pipelines.end()) {
        auto it = known->second.find(pipeline);
        if (it != known->second.end())
          state->second.layout = it->second;
      }
    }
  }
  d.bind_pipeline(command, point, pipeline);
}
void VKAPI_CALL draw_indexed(VkCommandBuffer command, uint32_t indexCount,
                             uint32_t instanceCount, uint32_t firstIndex,
                             int32_t vertexOffset, uint32_t firstInstance) {
  auto d = device_dispatch(reinterpret_cast<VkDevice>(command));
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::array<float, 528> geometry{};
  VkDescriptorSet hands{};
  bool log_draw = false;
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex);
    auto it = commands.find(command);
    if (it != commands.end() && it->second.layout) {
      auto &state = it->second;
      layout = state.layout;
      if (!state.sampled) {
        state.geometry = read_geometry();
        update_native_order(state.geometry);
        if (!prepare_hands(d, state)) return;
        state.sampled = true;
      }
      geometry = state.geometry;
      hands = state.hands;
      if (draw_logs < 4) {
        ++draw_logs;
        log_draw = true;
      }
    }
  }
  if (layout) {
    d.push(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, geometry.data());
    FN(d, vkCmdBindDescriptorSets)(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &hands, 0, nullptr);
  }
  if (log_draw)
    fprintf(stderr, "ft-passthrough: push draw enabled=%.0f center=%.3f,%.3f,%.3f\n",
            geometry[12], geometry[0], geometry[1], geometry[2]);
  d.draw_indexed(command, indexCount, instanceCount, firstIndex, vertexOffset,
                 firstInstance);
}

PFN_vkVoidFunction device_hook(const char *n) {
  if (!strcmp(n, "vkCreatePipelineLayout"))
    return (PFN_vkVoidFunction)create_layout;
  if (!strcmp(n, "vkDestroyPipelineLayout"))
    return (PFN_vkVoidFunction)destroy_layout;
  if (!strcmp(n, "vkDestroyPipeline"))
    return (PFN_vkVoidFunction)destroy_pipeline;
  if (!strcmp(n, "vkCmdBindPipeline"))
    return (PFN_vkVoidFunction)bind_pipeline;
  if (!strcmp(n, "vkCmdDrawIndexed"))
    return (PFN_vkVoidFunction)draw_indexed;
  if (!strcmp(n, "vkAllocateCommandBuffers"))
    return (PFN_vkVoidFunction)allocate_commands;
  if (!strcmp(n, "vkFreeCommandBuffers"))
    return (PFN_vkVoidFunction)free_commands;
  if (!strcmp(n, "vkBeginCommandBuffer"))
    return (PFN_vkVoidFunction)begin_command;
  if (!strcmp(n, "vkResetCommandBuffer"))
    return (PFN_vkVoidFunction)reset_command;
  if (!strcmp(n, "vkResetCommandPool"))
    return (PFN_vkVoidFunction)reset_pool;
  if (!strcmp(n, "vkDestroyCommandPool"))
    return (PFN_vkVoidFunction)destroy_pool;
  if (!strcmp(n, "vkDestroyShaderModule"))
    return (PFN_vkVoidFunction)destroy_module;
  if (!strcmp(n, "vkCreateGraphicsPipelines"))
    return (PFN_vkVoidFunction)create_pipelines;
  if (!strcmp(n, "vkCreateShaderModule"))
    return (PFN_vkVoidFunction)create_module;
  if (!strcmp(n, "vkDestroyDevice"))
    return (PFN_vkVoidFunction)destroy_device;
  return nullptr;
}

} // namespace
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance, const char *);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice, const char *);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice d, const char *n) {
  if (!n || !d)
    return nullptr;
  if (!strcmp(n, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  if (auto hook = device_hook(n))
    return hook;
  auto next = device_dispatch(d);
  return next.gdpa ? next.gdpa(d, n) : nullptr;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance i, const char *n) {
  if (!n)
    return nullptr;
  if (!strcmp(n, "vkGetInstanceProcAddr"))
    return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
  if (!strcmp(n, "vkCreateInstance"))
    return (PFN_vkVoidFunction)create_instance;
  if (!i)
    return nullptr;
  if (!strcmp(n, "vkGetDeviceProcAddr"))
    return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
  if (!strcmp(n, "vkDestroyInstance"))
    return (PFN_vkVoidFunction)destroy_instance;
  if (!strcmp(n, "vkCreateDevice"))
    return (PFN_vkVoidFunction)create_device;
  if (auto hook = device_hook(n))
    return hook;
  auto next = instance_dispatch(i);
  return next.gipa ? next.gipa(i, n) : nullptr;
}
static PFN_vkVoidFunction VKAPI_CALL physical_proc(VkInstance i,
                                                   const char *n) {
  if (!i || !n)
    return nullptr;
  auto d = instance_dispatch(i);
  return d.gpdpa ? d.gpdpa(i, n) : nullptr;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *p) {
  if (!p || p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT ||
      p->loaderLayerInterfaceVersion < 2)
    return VK_ERROR_INITIALIZATION_FAILED;
  p->loaderLayerInterfaceVersion = 2;
  p->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  p->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
  p->pfnGetPhysicalDeviceProcAddr = physical_proc;
  return VK_SUCCESS;
}
