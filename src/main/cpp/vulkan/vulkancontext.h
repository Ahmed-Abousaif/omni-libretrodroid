#ifndef LIBRETRODROID_VULKANCONTEXT_H
#define LIBRETRODROID_VULKANCONTEXT_H

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <functional>
#include <optional>

#include "../libretro/libretro-common/include/libretro.h"
#include "../libretro/libretro-common/include/libretro_vulkan.h"
#include "../log.h"

namespace libretrodroid {

class VulkanContext {
public:
    static VulkanContext& getInstance() {
        static VulkanContext instance;
        return instance;
    }

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool initialize(bool preferVulkan);
    void deinitialize();

    bool isVulkanRequested() const { return vulkanRequested; }
    bool isInitialized() const { return initialized; }

    void setNegotiationInterface(const struct retro_hw_render_context_negotiation_interface_vulkan& iface);
    bool initHwRender(struct retro_hw_render_callback* cb);
    bool ensureInitialized();
    const struct retro_hw_render_interface_vulkan* getInterface();

    // Device / Context handles
    VkInstance getInstanceHandle() const { return instance; }
    VkPhysicalDevice getGpu() const { return gpu; }
    VkDevice getDevice() const { return device; }
    VkQueue getQueue() const { return queue; }
    uint32_t getQueueFamilyIndex() const { return queueFamilyIndex; }

    // Surface / Window management
    void setNativeWindow(ANativeWindow* window);
    ANativeWindow* getNativeWindow() const { return nativeWindow; }
    VkSurfaceKHR getSurface() const { return surface; }
    void destroySurface();

    VkQueue getPresentationQueue() const { return presentationQueue != VK_NULL_HANDLE ? presentationQueue : queue; }
    uint32_t getPresentationQueueFamilyIndex() const { return presentationQueueFamilyIndex; }

    void setTargetFrameRate(float fps);
    void applyTargetFrameRate();
    float getTargetFrameRate() const { return targetFps; }

    // Libretro interface callback helpers
    void setImage(const struct retro_vulkan_image* img, uint32_t numSemaphores, const VkSemaphore* semaphores, uint32_t srcQueueFamily);
    void setCommandBuffers(uint32_t numCmd, const VkCommandBuffer* cmd);
    // Core callback. Waits until this slot's last submission has finished.
    // Does not reset the fence; the frontend owns reset.
    void waitSyncIndex();
    // Frontend only, before reusing the slot.
    void waitForSyncFence();
    void resetSyncFence();
    void waitAndResetSyncFence();
    uint32_t getSyncIndex();
    uint32_t getSyncIndexMask() const { return (1u << MAX_SYNC_INDEX) - 1u; }
    void setSignalSemaphore(VkSemaphore sem);

    std::mutex& getQueueMutex() { return queueMutex; }

    const struct retro_vulkan_image* getCurrentImage() const {
        if (currentImage != nullptr) return currentImage;
        if (hasLastImage) return &lastImage;
        return nullptr;
    }
    const std::vector<VkSemaphore>& getWaitSemaphores() const { return waitSemaphores; }
    const std::vector<VkCommandBuffer>& getCoreCommandBuffers() const { return coreCommandBuffers; }
    VkSemaphore getSignalSemaphore() const { return signalSemaphore; }
    VkFence getCurrentSyncFence();
    void clearPerFrameState();

    void setCurrentFenceInFlight();

    void advanceSyncIndex();

    retro_hw_context_reset_t getContextReset() const { return contextReset; }
    retro_hw_context_reset_t getContextDestroy() const { return contextDestroy; }
    bool isContextResetCalled() const { return contextResetCalled; }
    void setContextResetCalled(bool called) { contextResetCalled = called; }

    static constexpr uint32_t MAX_SYNC_INDEX = 3;

private:
    VulkanContext();
    ~VulkanContext();

    bool createInstance();
    bool selectPhysicalDevice();
    bool createDevice();

    static void callbackSetImage(void* handle, const struct retro_vulkan_image* image, uint32_t num_semaphores, const VkSemaphore* semaphores, uint32_t src_queue_family);
    static uint32_t callbackGetSyncIndex(void* handle);
    static uint32_t callbackGetSyncIndexMask(void* handle);
    static void callbackSetCommandBuffers(void* handle, uint32_t num_cmd, const VkCommandBuffer* cmd);
    static void callbackWaitSyncIndex(void* handle);
    static void callbackLockQueue(void* handle);
    static void callbackUnlockQueue(void* handle);
    static void callbackSetSignalSemaphore(void* handle, VkSemaphore semaphore);

    static VkInstance createInstanceWrapper(void* opaque, const VkInstanceCreateInfo* create_info);
    static VkDevice createDeviceWrapper(VkPhysicalDevice gpu, void* opaque, const VkDeviceCreateInfo* create_info);

private:
    bool vulkanRequested = false;
    bool initialized = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    VkQueue presentationQueue = VK_NULL_HANDLE;
    uint32_t presentationQueueFamilyIndex = 0;
    float targetFps = 0.0f;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ANativeWindow* nativeWindow = nullptr;

    bool coreCreatedDevice = false;
    std::optional<struct retro_hw_render_context_negotiation_interface_vulkan> negotiationInterface;

    struct retro_hw_render_interface_vulkan libretroInterface {};
    retro_hw_context_reset_t contextReset = nullptr;
    retro_hw_context_reset_t contextDestroy = nullptr;
    bool contextResetCalled = false;

    // Synchronization & Per-frame state
    std::mutex queueMutex;
    const struct retro_vulkan_image* currentImage = nullptr;
    struct retro_vulkan_image lastImage {};
    bool hasLastImage = false;
    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkCommandBuffer> coreCommandBuffers;
    VkSemaphore signalSemaphore = VK_NULL_HANDLE;

    // Fence wait/reset is shared by the core thread and the render thread.
    // Both used to reset the same fence. A second reset leaves it unsignaled
    // while a present semaphore is still in use, and Android then crashes in
    // Surface::queueBuffer. Only the frontend resets.
    mutable std::mutex syncMutex;
    uint32_t syncIndex = 0;
    uint32_t waitedIndex = 0;
    VkFence syncFences[MAX_SYNC_INDEX] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    bool fenceWaited[MAX_SYNC_INDEX] = { false, false, false };
};

} // namespace libretrodroid

#endif // LIBRETRODROID_VULKANCONTEXT_H
