#ifndef LIBRETRODROID_VULKANRENDERER_H
#define LIBRETRODROID_VULKANRENDERER_H

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <mutex>
#include <utility>

#include "../../vulkan/vulkancontext.h"
#include "../../vulkan/vulkanshaders.h"
#include "../../utils/rect.h"

namespace libretrodroid {

class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    bool initialize();
    void deinitialize();

    void updateScreenSize(uint32_t width, uint32_t height);
    void updateAspectRatio(float aspectRatio);
    void updateViewport(Rect viewport);
    void updateViewportAlignment(unsigned int alignment);
    // MAILBOX is used when FPSSync is pacing, or when present must not block the queue.
    void setAllowMailbox(bool allow);
    bool isUsingMailbox() const { return usingMailbox; }

    void setLinearFiltering(bool linear);
    bool isLinearFiltering() const { return useLinearFiltering; }

    void setCacheDirectory(const std::string& dir);
    void savePipelineCache();
    void onTrimMemory(int level);

    bool renderFrame();

    std::pair<float, float> getRelativePosition(float touchX, float touchY);

    double getLastGpuTimeMs() const { return lastGpuTimeMs; }

    bool isReady() const { return initialized && swapchain != VK_NULL_HANDLE; }

private:
    bool initPipelineCache();
    void destroyPipelineCache();
    std::string getPipelineCacheFilePath() const;
    bool createSwapchain();
    void destroySwapchain();
    bool createRenderPass();
    bool createPipeline();
    bool createDescriptorObjects();
    bool createCommandBuffers();
    bool createSyncObjects();
    void ensureRenderFinishedSemaphores(uint32_t count);
    void destroyRenderFinishedSemaphores();
    void waitQueueIdleLocked();
    bool surfaceMismatch() const;

    void initTimestampQueries();
    void cleanupTimestampQueries();

    struct PushConstants {
        float transform[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
        float offset[2] = { 0.0f, 0.0f };
    };
    void recomputePushConstants();

private:
    bool initialized = false;
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
    float currentAspectRatio = 4.0f / 3.0f;
    Rect currentViewport = Rect(0.0f, 0.0f, 1.0f, 1.0f);
    unsigned int viewportAlignment = 0;

    VkSurfaceTransformFlagBitsKHR currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkFormat swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D swapchainExtent { 0, 0 };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> swapchainFramebuffers;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::string cacheDirectory;
    bool pipelineCacheDirty = false;

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets;
    VkSampler samplerLinear = VK_NULL_HANDLE;
    VkSampler samplerNearest = VK_NULL_HANDLE;
    bool useLinearFiltering = true;

    // Descriptor set update caching (avoids redundant vkUpdateDescriptorSets every frame)
    VkImageView lastBoundImageView[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkSampler lastBoundSampler[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkImageLayout lastBoundImageLayout[3] = { VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED };

    // Matches VulkanContext::MAX_SYNC_INDEX.
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

    // Per-frame-in-flight transient command pools (fast pool reset, zero allocator fragmentation)
    VkCommandPool commandPools[MAX_FRAMES_IN_FLIGHT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };

    PushConstants cachedPush {};
    bool pushConstantsDirty = true;
    uint32_t currentFrame = 0;
    bool allowMailbox = false;
    bool usingMailbox = false;
    bool needsRecreate = false;
    VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    // One present semaphore per swapchain image, not per frame-in-flight slot.
    std::vector<VkSemaphore> renderFinishedSemaphores;

    // GPU timestamp profiling
    VkQueryPool timestampQueryPool = VK_NULL_HANDLE;
    float timestampPeriod = 0.0f;
    uint32_t timestampValidBits = 0;
    bool timestampsSupported = false;
    double lastGpuTimeMs = -1.0;
    bool queryInFlight[MAX_FRAMES_IN_FLIGHT] = { false, false, false };
};

} // namespace libretrodroid

#endif // LIBRETRODROID_VULKANRENDERER_H
