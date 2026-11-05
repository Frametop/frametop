// オーバーレイ用 Vulkan テクスチャの実装。
#include "vk_texture.h"

#include "openvr.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace {


/**
 * 空白区切りの拡張名の文字列を分ける（OpenVR が返す形式）。
 * @param text 拡張名の並び
 * @return 拡張名の一覧
 */
std::vector<std::string> splitNames(const std::string& text) {
    std::vector<std::string> names;
    std::istringstream in(text);
    std::string name;
    while (in >> name) names.push_back(name);
    return names;
}

/**
 * 要求された拡張のうち、実際に使えるものだけ残す（使えないものはログに出す）。
 * @param wanted 要求された拡張名
 * @param available 使える拡張の一覧
 * @param kind ログ用の種類名
 * @return 有効にする拡張名
 */
std::vector<std::string> filterSupported(const std::vector<std::string>& wanted,
                                         const std::vector<VkExtensionProperties>& available, const char* kind) {
    std::vector<std::string> result;
    for (const auto& name : wanted) {
        bool found = false;
        for (const auto& ext : available) {
            if (name == ext.extensionName) found = true;
        }
        if (found) {
            result.push_back(name);
        } else {
            std::fprintf(stderr, "[Vulkan] %s 拡張 %s は使えないので外します\n", kind, name.c_str());
        }
    }
    return result;
}

/**
 * std::string の一覧を const char* の一覧にする（Vulkan の作成情報用）。
 * @param names 名前の一覧（呼び出し中は生きていること）
 * @return ポインタの一覧
 */
std::vector<const char*> toPointers(const std::vector<std::string>& names) {
    std::vector<const char*> pointers;
    for (const auto& name : names) pointers.push_back(name.c_str());
    return pointers;
}

/**
 * VkResult を失敗理由の文字列にする。
 * @param what 何をしたときか
 * @param result 結果
 * @return 理由の文字列
 */
std::string vkError(const char* what, VkResult result) {
    return std::string(what) + " に失敗 (VkResult " + std::to_string(static_cast<int>(result)) + ")";
}

}  // namespace

VulkanContext::~VulkanContext() {
    destroy();
}

bool VulkanContext::init(std::string& error) {
    destroy();

    // インスタンス: OpenVR が要求する拡張を有効にする
    char buffer[4096] = {};
    vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buffer, sizeof(buffer));
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> instanceExts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, instanceExts.data());
    const std::vector<std::string> instanceNames = filterSupported(splitNames(buffer), instanceExts, "インスタンス");
    const std::vector<const char*> instancePtrs = toPointers(instanceNames);

    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "frame-perf-overlay";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instancePtrs.size());
    instanceInfo.ppEnabledExtensionNames = instancePtrs.data();
    VkResult result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        error = vkError("vkCreateInstance", result);
        instance_ = VK_NULL_HANDLE;
        return false;
    }

    // GPU: HMD が使っているものを OpenVR に聞く（取れなければ最初の GPU）
    uint64_t outputDevice = 0;
    vr::VRSystem()->GetOutputDevice(&outputDevice, vr::TextureType_Vulkan, reinterpret_cast<VkInstance_T*>(instance_));
    physicalDevice_ = reinterpret_cast<VkPhysicalDevice>(outputDevice);
    if (physicalDevice_ == VK_NULL_HANDLE) {
        uint32_t gpuCount = 1;
        VkPhysicalDevice first = VK_NULL_HANDLE;
        vkEnumeratePhysicalDevices(instance_, &gpuCount, &first);
        physicalDevice_ = first;
    }
    if (physicalDevice_ == VK_NULL_HANDLE) {
        error = "Vulkan の GPU が見つかりません";
        destroy();
        return false;
    }

    // キュー: グラフィックスが使えるファミリー（転送も必ずできる）
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
    bool foundFamily = false;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily_ = i;
            foundFamily = true;
            break;
        }
    }
    if (!foundFamily) {
        error = "グラフィックス用のキューがありません";
        destroy();
        return false;
    }

    // デバイス: OpenVR が要求する拡張を有効にする
    std::memset(buffer, 0, sizeof(buffer));
    vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(reinterpret_cast<VkPhysicalDevice_T*>(physicalDevice_),
                                                         buffer, sizeof(buffer));
    count = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> deviceExts(count);
    vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &count, deviceExts.data());
    const std::vector<std::string> deviceNames = filterSupported(splitNames(buffer), deviceExts, "デバイス");
    const std::vector<const char*> devicePtrs = toPointers(deviceNames);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(devicePtrs.size());
    deviceInfo.ppEnabledExtensionNames = devicePtrs.data();
    result = vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_);
    if (result != VK_SUCCESS) {
        error = vkError("vkCreateDevice", result);
        device_ = VK_NULL_HANDLE;
        destroy();
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
    if (result != VK_SUCCESS) {
        error = vkError("vkCreateCommandPool", result);
        commandPool_ = VK_NULL_HANDLE;
        destroy();
        return false;
    }

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    std::fprintf(stderr, "[Vulkan] %s を使います（インスタンス拡張 %zu 個、デバイス拡張 %zu 個）\n", props.deviceName,
                 instanceNames.size(), deviceNames.size());
    return true;
}

void VulkanContext::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    commandPool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

uint32_t VulkanContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory {};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return UINT32_MAX;
}

OverlayTexture::~OverlayTexture() {
    destroy();
}

bool OverlayTexture::create(VulkanContext& context, int width, int height, std::string& error, VkFormat format) {
    destroy();
    context_ = &context;
    // FrameKeyboard: the caller supplies the format matching its pixel bytes.
    format_ = format;
    width_ = width;
    height_ = height;
    const VkDevice device = context.device();

    // 画像 2 枚（転送先にも転送元にもなり、コンポジタがサンプルできる）
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo imageInfo {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format_;
        imageInfo.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(device, &imageInfo, nullptr, &images_[i]);
        if (result != VK_SUCCESS) {
            error = vkError("vkCreateImage", result);
            images_[i] = VK_NULL_HANDLE;
            destroy();
            return false;
        }
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(device, images_[i], &req);
        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = context.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        result = vkAllocateMemory(device, &alloc, nullptr, &imageMemory_[i]);
        if (result != VK_SUCCESS) {
            error = vkError("vkAllocateMemory(image)", result);
            imageMemory_[i] = VK_NULL_HANDLE;
            destroy();
            return false;
        }
        // FrameKeyboard: stop on allocation/command errors before using an invalid resource.
        if (const auto checked = vkBindImageMemory(device, images_[i], imageMemory_[i], 0); checked != VK_SUCCESS) {
            error = vkError("vkBindImageMemory", checked);
            destroy();
            return false;
        }
        imageUsed_[i] = false;
    }

    // CPU から書き込む転送用バッファ（常にマップしておく）
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
    VkBufferCreateInfo bufferInfo {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &staging_);
    if (result != VK_SUCCESS) {
        error = vkError("vkCreateBuffer", result);
        staging_ = VK_NULL_HANDLE;
        destroy();
        return false;
    }
    VkMemoryRequirements req {};
    vkGetBufferMemoryRequirements(device, staging_, &req);
    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = context.findMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    result = vkAllocateMemory(device, &alloc, nullptr, &stagingMemory_);
    if (result != VK_SUCCESS) {
        error = vkError("vkAllocateMemory(staging)", result);
        stagingMemory_ = VK_NULL_HANDLE;
        destroy();
        return false;
    }
    // FrameKeyboard: stop on allocation/command errors before using an invalid resource.
    if (const auto checked = vkBindBufferMemory(device, staging_, stagingMemory_, 0); checked != VK_SUCCESS) {
        error = vkError("vkBindBufferMemory", checked);
        destroy();
        return false;
    }
    // FrameKeyboard: stop on allocation/command errors before using an invalid resource.
    if (const auto checked = vkMapMemory(device, stagingMemory_, 0, bytes, 0, &stagingMapped_); checked != VK_SUCCESS) {
        error = vkError("vkMapMemory", checked);
        destroy();
        return false;
    }

    VkCommandBufferAllocateInfo cmdInfo {};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = context.commandPool();
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    // FrameKeyboard: stop on allocation/command errors before using an invalid resource.
    if (const auto checked = vkAllocateCommandBuffers(device, &cmdInfo, &commandBuffer_); checked != VK_SUCCESS) {
        error = vkError("vkAllocateCommandBuffers", checked);
        destroy();
        return false;
    }

    VkFenceCreateInfo fenceInfo {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(device, &fenceInfo, nullptr, &fence_);
    if (result != VK_SUCCESS) {
        error = vkError("vkCreateFence", result);
        fence_ = VK_NULL_HANDLE;
        destroy();
        return false;
    }
    next_ = 0;
    return true;
}

void OverlayTexture::destroy() {
    if (context_ == nullptr || !context_->ready()) {
        context_ = nullptr;
        return;
    }
    const VkDevice device = context_->device();
    vkDeviceWaitIdle(device);
    if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device, fence_, nullptr);
    if (commandBuffer_ != VK_NULL_HANDLE) vkFreeCommandBuffers(device, context_->commandPool(), 1, &commandBuffer_);
    if (stagingMapped_ != nullptr) vkUnmapMemory(device, stagingMemory_);
    if (staging_ != VK_NULL_HANDLE) vkDestroyBuffer(device, staging_, nullptr);
    if (stagingMemory_ != VK_NULL_HANDLE) vkFreeMemory(device, stagingMemory_, nullptr);
    for (int i = 0; i < 2; ++i) {
        if (images_[i] != VK_NULL_HANDLE) vkDestroyImage(device, images_[i], nullptr);
        if (imageMemory_[i] != VK_NULL_HANDLE) vkFreeMemory(device, imageMemory_[i], nullptr);
        images_[i] = VK_NULL_HANDLE;
        imageMemory_[i] = VK_NULL_HANDLE;
        imageUsed_[i] = false;
    }
    fence_ = VK_NULL_HANDLE;
    commandBuffer_ = VK_NULL_HANDLE;
    stagingMapped_ = nullptr;
    staging_ = VK_NULL_HANDLE;
    stagingMemory_ = VK_NULL_HANDLE;
    context_ = nullptr;
}

bool OverlayTexture::update(uint64_t overlayHandle, const uint8_t* rgba, std::string& error) {
    if (!ready()) {
        error = "テクスチャが作られていません";
        return false;
    }
    const int index = next_;
    next_ = 1 - next_;
    const VkImage image = images_[index];
    std::memcpy(stagingMapped_, rgba, static_cast<size_t>(width_) * height_ * 4);

    // FrameKeyboard: stop on allocation/command errors before using an invalid resource.
    if (const auto checked = vkResetCommandBuffer(commandBuffer_, 0); checked != VK_SUCCESS) {
        error = vkError("vkResetCommandBuffer", checked);
        return false;
    }
    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    // FrameKeyboard: stop on allocation/command errors before using an invalid resource.
    if (const auto checked = vkBeginCommandBuffer(commandBuffer_, &begin); checked != VK_SUCCESS) {
        error = vkError("vkBeginCommandBuffer", checked);
        return false;
    }

    // 転送先のレイアウトへ（前回は TRANSFER_SRC で渡している）
    VkImageMemoryBarrier toDst {};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.srcAccessMask = imageUsed_[index] ? VK_ACCESS_TRANSFER_READ_BIT : 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = imageUsed_[index] ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
    vkCmdCopyBufferToImage(commandBuffer_, staging_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // OpenVR に渡すときは TRANSFER_SRC_OPTIMAL にしておく決まり
    VkImageMemoryBarrier toSrc = toDst;
    toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toSrc);
    // FrameKeyboard: stop on allocation/command errors before using an invalid resource.
    if (const auto checked = vkEndCommandBuffer(commandBuffer_); checked != VK_SUCCESS) {
        error = vkError("vkEndCommandBuffer", checked);
        return false;
    }

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    // FrameKeyboard: stop on allocation/command errors before using an invalid resource.
    if (const auto checked = vkResetFences(context_->device(), 1, &fence_); checked != VK_SUCCESS) {
        error = vkError("vkResetFences", checked);
        return false;
    }
    VkResult result = vkQueueSubmit(context_->queue(), 1, &submit, fence_);
    if (result != VK_SUCCESS) {
        error = vkError("vkQueueSubmit", result);
        return false;
    }
    // 転送が終わるまで待つ（数百 µs。待っている間 CPU は眠る）
    result = vkWaitForFences(context_->device(), 1, &fence_, VK_TRUE, 1000000000ull);
    if (result != VK_SUCCESS) {
        error = vkError("vkWaitForFences", result);
        return false;
    }
    imageUsed_[index] = true;

    vr::VRVulkanTextureData_t data {};
    data.m_nImage = reinterpret_cast<uint64_t>(image);
    data.m_pDevice = reinterpret_cast<VkDevice_T*>(context_->device());
    data.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(context_->physicalDevice());
    data.m_pInstance = reinterpret_cast<VkInstance_T*>(context_->instance());
    data.m_pQueue = reinterpret_cast<VkQueue_T*>(context_->queue());
    data.m_nQueueFamilyIndex = context_->queueFamily();
    data.m_nWidth = static_cast<uint32_t>(width_);
    data.m_nHeight = static_cast<uint32_t>(height_);
    data.m_nFormat = format_;
    data.m_nSampleCount = 1;
    // Gamma = 画素値をそのまま表示に使う（これまでの SetOverlayRaw と同じ見え方）
    vr::Texture_t texture {&data, vr::TextureType_Vulkan, vr::ColorSpace_Gamma};
    const vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(overlayHandle, &texture);
    if (overlayError != vr::VROverlayError_None) {
        error = std::string("SetOverlayTexture: ") + vr::VROverlay()->GetOverlayErrorNameFromEnum(overlayError);
        return false;
    }
    return true;
}
