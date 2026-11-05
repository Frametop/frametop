// オーバーレイ用の Vulkan テクスチャ（SetOverlayTexture で渡す）。
// SetOverlayRaw は差し替えのたびに 15〜40ms テクスチャが無い状態になり（ちらつき）、
// 共有メモリ経由なので終了時のコンポジタの SIGBUS の疑いもあるため使わない。
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

/**
 * OpenVR が指定する GPU・拡張で作った Vulkan のデバイス一式。
 * OpenVR の初期化後に作り、VR_Shutdown の後で壊す。
 */
class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    /**
     * インスタンス・デバイス・キュー・コマンドプールを作る（OpenVR が初期化済みであること）。
     * @param error 失敗したときの理由
     * @return 成功したら true
     */
    bool init(std::string& error);

    /** 作ったものをすべて壊す。何も作っていなければ何もしない。 */
    void destroy();

    /**
     * 条件に合うメモリの種類を探す。
     * @param typeBits 使える種類のビット（VkMemoryRequirements::memoryTypeBits）
     * @param properties 必要な性質
     * @return 種類の番号。見つからなければ UINT32_MAX
     */
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;

    /** @return 初期化済みなら true */
    bool ready() const { return device_ != VK_NULL_HANDLE; }

    /** @return Vulkan のインスタンス */
    VkInstance instance() const { return instance_; }
    /** @return HMD をつないでいる GPU */
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    /** @return 論理デバイス */
    VkDevice device() const { return device_; }
    /** @return 転送に使うキュー */
    VkQueue queue() const { return queue_; }
    /** @return キューファミリーの番号 */
    uint32_t queueFamily() const { return queueFamily_; }
    /** @return コマンドプール */
    VkCommandPool commandPool() const { return commandPool_; }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
};

/**
 * 1 枚のオーバーレイに送るテクスチャ。画像を 2 枚持って交互に書き込む
 * （コンポジタが前の画像を読んでいる最中に書き換えないため）。
 */
class OverlayTexture {
public:
    OverlayTexture() = default;
    ~OverlayTexture();
    OverlayTexture(const OverlayTexture&) = delete;
    OverlayTexture& operator=(const OverlayTexture&) = delete;

    /**
     * 画像・転送用バッファ・コマンドバッファを作る。
     * @param context 初期化済みの Vulkan
     * @param width 幅（px）
     * @param height 高さ（px）
     * @param error 失敗したときの理由
     * @return 成功したら true
     */
    bool create(VulkanContext& context, int width, int height, std::string& error,
                VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);

    /** 作ったものを壊す（GPU の処理が終わるのを待ってから）。 */
    void destroy();

    /**
     * RGBA の画素を GPU の画像に写し、SetOverlayTexture でオーバーレイに渡す。
     * 転送の完了を待ってから渡すので、渡した時点で画像は完成している。
     * @param overlayHandle vr::VROverlayHandle_t
     * @param rgba Pixel bytes matching create() format (width * height * 4).
     * FrameKeyboard: the caller configures overlay alpha to match these bytes.
     * @param error 失敗したときの理由
     * @return 成功したら true
     */
    bool update(uint64_t overlayHandle, const uint8_t* rgba, std::string& error);

    /** @return 作成済みなら true */
    bool ready() const { return fence_ != VK_NULL_HANDLE; }

private:
    VulkanContext* context_ = nullptr;
    // FrameKeyboard: allow Cairo-native BGRA while retaining RGBA as the default.
    VkFormat format_ = VK_FORMAT_R8G8B8A8_UNORM;
    int width_ = 0;
    int height_ = 0;
    VkImage images_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory imageMemory_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool imageUsed_[2] = {false, false};  ///< 一度でも書いたか（レイアウト遷移の元を決める）
    int next_ = 0;                        ///< 次に書く画像
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void* stagingMapped_ = nullptr;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};
