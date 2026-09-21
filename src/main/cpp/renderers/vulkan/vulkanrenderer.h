#ifndef LIBRETRODROID_VULKANRENDERER_H
#define LIBRETRODROID_VULKANRENDERER_H

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <mutex>

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

    bool renderFrame();

    bool isReady() const { return initialized && swapchain != VK_NULL_HANDLE; }

private:
    bool createSwapchain();
    void destroySwapchain();
    bool createRenderPass();
    bool createPipeline();
    bool createDescriptorObjects();
    bool createCommandBuffers();
    bool createSyncObjects();

    struct PushConstants {
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
    };

private:
    bool initialized = false;
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
    float currentAspectRatio = 4.0f / 3.0f;
    Rect currentViewport = Rect(0.0f, 0.0f, 1.0f, 1.0f);
    unsigned int viewportAlignment = 0;

    VkFormat swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D swapchainExtent { 0, 0 };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> swapchainFramebuffers;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets;
    VkSampler sampler = VK_NULL_HANDLE;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;

    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
    uint32_t currentFrame = 0;
    VkSemaphore imageAvailableSemaphores[MAX_FRAMES_IN_FLIGHT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkSemaphore renderFinishedSemaphores[MAX_FRAMES_IN_FLIGHT] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
};

} // namespace libretrodroid

#endif // LIBRETRODROID_VULKANRENDERER_H
