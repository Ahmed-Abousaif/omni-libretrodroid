#ifndef LIBRETRODROID_VULKANRENDERER_H
#define LIBRETRODROID_VULKANRENDERER_H

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <mutex>
#include <utility>
#include <thread>
#include <condition_variable>
#include <atomic>

#include "../../vulkan/vulkancontext.h"
#include "../../vulkan/vulkanshaders.h"
#include "../../utils/rect.h"

#ifndef VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
#define VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME "VK_KHR_push_descriptor"
#endif

#ifndef VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR
#define VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR 0x00000001
#endif

#ifndef VK_STRUCTURE_TYPE_RENDERING_INFO_KHR
#define VK_STRUCTURE_TYPE_RENDERING_INFO_KHR static_cast<VkStructureType>(1000044000)
#endif

#ifndef VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR
#define VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR static_cast<VkStructureType>(1000044001)
#endif

#ifndef VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR
#define VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR static_cast<VkStructureType>(1000044002)
#endif

#ifndef VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR
#define VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR static_cast<VkStructureType>(1000314004)
#endif

#ifndef VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR static_cast<VkStructureType>(1000314000)
#endif

#ifndef VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR
#define VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR static_cast<VkStructureType>(1000314001)
#endif

#ifndef VK_KHR_dynamic_rendering
typedef struct VkRenderingAttachmentInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkImageView imageView;
    VkImageLayout imageLayout;
    VkResolveModeFlagBits resolveMode;
    VkImageView resolveImageView;
    VkImageLayout resolveImageLayout;
    VkAttachmentLoadOp loadOp;
    VkAttachmentStoreOp storeOp;
    VkClearValue clearValue;
} VkRenderingAttachmentInfoKHR;

typedef struct VkRenderingInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkRenderingFlags flags;
    VkRect2D renderArea;
    uint32_t layerCount;
    uint32_t viewMask;
    uint32_t colorAttachmentCount;
    const VkRenderingAttachmentInfoKHR* pColorAttachments;
    const VkRenderingAttachmentInfoKHR* pDepthAttachment;
    const VkRenderingAttachmentInfoKHR* pStencilAttachment;
} VkRenderingInfoKHR;

typedef struct VkPipelineRenderingCreateInfoKHR {
    VkStructureType sType;
    const void* pNext;
    uint32_t viewMask;
    uint32_t colorAttachmentCount;
    const VkFormat* pColorAttachmentFormats;
    VkFormat depthAttachmentFormat;
    VkFormat stencilAttachmentFormat;
} VkPipelineRenderingCreateInfoKHR;

typedef void (VKAPI_PTR *PFN_vkCmdBeginRenderingKHR)(VkCommandBuffer commandBuffer, const VkRenderingInfoKHR* pRenderingInfo);
typedef void (VKAPI_PTR *PFN_vkCmdEndRenderingKHR)(VkCommandBuffer commandBuffer);
#endif

#ifndef VK_KHR_push_descriptor
typedef void (VKAPI_PTR *PFN_vkCmdPushDescriptorSetKHR)(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t set,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites);
#endif

#ifndef VK_KHR_synchronization2
typedef uint64_t VkPipelineStageFlags2KHR;
typedef uint64_t VkAccessFlags2KHR;

typedef struct VkCommandBufferSubmitInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkCommandBuffer commandBuffer;
    uint32_t deviceMask;
} VkCommandBufferSubmitInfoKHR;

typedef struct VkSemaphoreSubmitInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkSemaphore semaphore;
    uint64_t value;
    VkPipelineStageFlags2KHR stageMask;
    uint32_t deviceIndex;
} VkSemaphoreSubmitInfoKHR;

typedef struct VkSubmitInfo2KHR {
    VkStructureType sType;
    const void* pNext;
    VkSubmitFlags flags;
    uint32_t waitSemaphoreInfoCount;
    const VkSemaphoreSubmitInfoKHR* pWaitSemaphoreInfos;
    uint32_t commandBufferInfoCount;
    const VkCommandBufferSubmitInfoKHR* pCommandBufferInfos;
    uint32_t signalSemaphoreInfoCount;
    const VkSemaphoreSubmitInfoKHR* pSignalSemaphoreInfos;
} VkSubmitInfo2KHR;

typedef VkResult (VKAPI_PTR *PFN_vkQueueSubmit2KHR)(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2KHR* pSubmits, VkFence fence);
#endif

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

    // Asynchronous presentation pipeline
    void setAsyncPresentation(bool enable);
    bool isAsyncPresentationEnabled() const { return asyncPresentation; }
    void requestRenderAsync();
    void flushAsync();

    std::pair<float, float> getRelativePosition(float touchX, float touchY);

    double getLastGpuTimeMs() const { return lastGpuTimeMs; }

    bool isReady() const { return initialized && swapchain != VK_NULL_HANDLE; }

    bool isDirty() const { return isDirtyFlag; }
    void markDirty() { isDirtyFlag = true; }
    void clearDirty() { isDirtyFlag = false; }

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

    void startAsyncWorker();
    void stopAsyncWorker();
    void asyncWorkerLoop();

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

    // Matches VulkanContext::MAX_SYNC_INDEX.
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

    // Per-frame-in-flight transient command pools (fast pool reset, zero allocator fragmentation)
    VkCommandPool commandPools[MAX_FRAMES_IN_FLIGHT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkCommandBuffer commandBuffers[MAX_FRAMES_IN_FLIGHT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };

    PushConstants cachedPush {};
    bool pushConstantsDirty = true;
    bool isDirtyFlag = true;
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

    // Dynamic rendering & Modern extension function pointers
    PFN_vkCmdBeginRenderingKHR pfnCmdBeginRendering = nullptr;
    PFN_vkCmdEndRenderingKHR pfnCmdEndRendering = nullptr;
    PFN_vkCmdPushDescriptorSetKHR pfnCmdPushDescriptorSetKHR = nullptr;
    PFN_vkQueueSubmit2KHR pfnQueueSubmit2 = nullptr;

    bool dynamicRenderingSupported = false;
    bool pushDescriptorSupported = false;
    bool synchronization2Supported = false;
    bool isLetterboxed = true;

    // Asynchronous Presentation Worker pipeline
    std::thread asyncWorkerThread;
    std::mutex asyncMutex;
    std::condition_variable asyncCv;
    std::condition_variable asyncDoneCv;
    std::atomic<bool> asyncWorkerRunning { false };
    std::atomic<bool> asyncFramePending { false };
    std::atomic<bool> asyncFrameInFlight { false };
    bool asyncPresentation = true;
};

} // namespace libretrodroid

#endif // LIBRETRODROID_VULKANRENDERER_H
