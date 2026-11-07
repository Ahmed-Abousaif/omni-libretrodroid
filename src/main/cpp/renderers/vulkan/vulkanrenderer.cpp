#include "vulkanrenderer.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <unistd.h>
#include <pthread.h>
#include "../../environment.h"
#include "../../stats.h"
#include "../../utils/adpf.h"

namespace libretrodroid {

VulkanRenderer::VulkanRenderer() = default;

VulkanRenderer::~VulkanRenderer() {
    deinitialize();
}

bool VulkanRenderer::initialize() {
    if (initialized) {
        return true;
    }

    VkDevice device = VulkanContext::getInstance().getDevice();
    VkSurfaceKHR surface = VulkanContext::getInstance().getSurface();
    if (device == VK_NULL_HANDLE || surface == VK_NULL_HANDLE) {
        LOGI("VulkanRenderer::initialize waiting for device (%p) and surface (%p)", device, (void*)(uintptr_t)surface);
        return false;
    }

    LOGI("Initializing VulkanRenderer on device %p...", device);

    dynamicRenderingSupported = VulkanContext::getInstance().isDynamicRenderingSupported();
    pushDescriptorSupported = VulkanContext::getInstance().isPushDescriptorSupported();
    synchronization2Supported = VulkanContext::getInstance().isSynchronization2Supported();

    pfnCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR")
    );
    if (!pfnCmdBeginRendering) {
        pfnCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
            vkGetDeviceProcAddr(device, "vkCmdBeginRendering")
        );
    }
    pfnCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR")
    );
    if (!pfnCmdEndRendering) {
        pfnCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
            vkGetDeviceProcAddr(device, "vkCmdEndRendering")
        );
    }
    if (!pfnCmdBeginRendering || !pfnCmdEndRendering) {
        dynamicRenderingSupported = false;
    }

    pfnCmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR")
    );
    if (!pfnCmdPushDescriptorSetKHR) {
        pushDescriptorSupported = false;
    }

    pfnQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
        vkGetDeviceProcAddr(device, "vkQueueSubmit2KHR")
    );
    if (!pfnQueueSubmit2) {
        pfnQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2KHR>(
            vkGetDeviceProcAddr(device, "vkQueueSubmit2")
        );
    }
    if (!pfnQueueSubmit2) {
        synchronization2Supported = false;
    }

    LOGI("VulkanRenderer pipeline features: dynamicRendering=%d, pushDescriptor=%d, synchronization2=%d",
         dynamicRenderingSupported, pushDescriptorSupported, synchronization2Supported);

    if (!createSwapchain()) {
        LOGE("Failed to create Vulkan swapchain");
        return false;
    }

    if (!dynamicRenderingSupported) {
        if (!createRenderPass()) {
            LOGE("Failed to create Vulkan render pass");
            return false;
        }

        // Now that render pass exists, create swapchain framebuffers
        destroySwapchain();
        if (!createSwapchain()) {
            LOGE("Failed to recreate Vulkan swapchain framebuffers");
            return false;
        }
    }

    initPipelineCache();

    if (!createPipeline()) {
        LOGE("Failed to create Vulkan graphics pipeline");
        return false;
    }

    if (!createDescriptorObjects()) {
        LOGE("Failed to create Vulkan descriptor objects");
        return false;
    }

    if (!createCommandBuffers()) {
        LOGE("Failed to create Vulkan command buffers");
        return false;
    }

    if (!createSyncObjects()) {
        LOGE("Failed to create Vulkan sync objects");
        return false;
    }

    initTimestampQueries();

    if (asyncPresentation) {
        startAsyncWorker();
    }

    initialized = true;
    LOGI("VulkanRenderer successfully initialized (asyncPresentation=%d)!", asyncPresentation);
    return true;
}

void VulkanRenderer::waitQueueIdleLocked() {
    VkQueue queue = VulkanContext::getInstance().getQueue();
    if (queue == VK_NULL_HANDLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(VulkanContext::getInstance().getQueueMutex());
    vkQueueWaitIdle(queue);
}

void VulkanRenderer::setAllowMailbox(bool allow) {
    if (allowMailbox == allow) {
        return;
    }
    allowMailbox = allow;
    isDirtyFlag = true;
    if (initialized) {
        needsRecreate = true;
    }
}

void VulkanRenderer::setLinearFiltering(bool linear) {
    if (useLinearFiltering != linear) {
        useLinearFiltering = linear;
        isDirtyFlag = true;
        LOGI("VulkanRenderer filtering updated to %s", linear ? "LINEAR" : "NEAREST");
    }
}

void VulkanRenderer::setCacheDirectory(const std::string& dir) {
    cacheDirectory = dir;
}

std::string VulkanRenderer::getPipelineCacheFilePath() const {
    std::string baseDir = cacheDirectory;
    if (baseDir.empty()) {
        baseDir = Environment::getInstance().getSavesDirectory();
    }
    if (baseDir.empty()) {
        return "";
    }
    if (baseDir.back() != '/' && baseDir.back() != '\\') {
        baseDir += "/";
    }
    return baseDir + "vulkan_pipeline_cache.bin";
}

bool VulkanRenderer::initPipelineCache() {
    VkPhysicalDevice gpu = VulkanContext::getInstance().getGpu();
    VkDevice device = VulkanContext::getInstance().getDevice();
    if (gpu == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        return false;
    }

    if (pipelineCache != VK_NULL_HANDLE) {
        return true;
    }

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(gpu, &props);

    std::vector<uint8_t> cacheData;
    std::string path = getPipelineCacheFilePath();

    if (!path.empty()) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file.is_open()) {
            std::streamsize size = file.tellg();
            if (size >= static_cast<std::streamsize>(sizeof(VkPipelineCacheHeaderVersionOne))) {
                file.seekg(0, std::ios::beg);
                cacheData.resize(static_cast<size_t>(size));
                if (file.read(reinterpret_cast<char*>(cacheData.data()), size)) {
                    const auto* header = reinterpret_cast<const VkPipelineCacheHeaderVersionOne*>(cacheData.data());
                    bool valid = (header->headerSize >= sizeof(VkPipelineCacheHeaderVersionOne)) &&
                                 (header->headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE) &&
                                 (header->vendorID == props.vendorID) &&
                                 (header->deviceID == props.deviceID) &&
                                 (memcmp(header->pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0);
                    if (valid) {
                        LOGI("Loaded valid Vulkan pipeline cache from %s (%zu bytes)", path.c_str(), cacheData.size());
                    } else {
                        LOGW("Stale or invalid Vulkan pipeline cache in %s (vendor=0x%x vs 0x%x, device=0x%x vs 0x%x). Discarding.",
                             path.c_str(), header->vendorID, props.vendorID, header->deviceID, props.deviceID);
                        cacheData.clear();
                        file.close();
                        std::remove(path.c_str());
                    }
                } else {
                    cacheData.clear();
                }
            } else {
                LOGW("Vulkan pipeline cache file %s too small (%zu bytes). Discarding.", path.c_str(), static_cast<size_t>(size));
                cacheData.clear();
                file.close();
                std::remove(path.c_str());
            }
        }
    }

    VkPipelineCacheCreateInfo pci {};
    pci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (!cacheData.empty()) {
        pci.initialDataSize = cacheData.size();
        pci.pInitialData = cacheData.data();
    }

    VkResult res = vkCreatePipelineCache(device, &pci, nullptr, &pipelineCache);
    if (res != VK_SUCCESS) {
        LOGW("vkCreatePipelineCache with initial data failed: %d. Retrying with empty cache.", res);
        pci.initialDataSize = 0;
        pci.pInitialData = nullptr;
        res = vkCreatePipelineCache(device, &pci, nullptr, &pipelineCache);
        if (res != VK_SUCCESS) {
            LOGE("vkCreatePipelineCache failed: %d", res);
            pipelineCache = VK_NULL_HANDLE;
            return false;
        }
    }

    return true;
}

void VulkanRenderer::savePipelineCache() {
    VkDevice device = VulkanContext::getInstance().getDevice();
    if (device == VK_NULL_HANDLE || pipelineCache == VK_NULL_HANDLE || !pipelineCacheDirty) {
        return;
    }

    size_t dataSize = 0;
    VkResult res = vkGetPipelineCacheData(device, pipelineCache, &dataSize, nullptr);
    if (res != VK_SUCCESS || dataSize <= sizeof(VkPipelineCacheHeaderVersionOne)) {
        return;
    }

    std::vector<uint8_t> data(dataSize);
    res = vkGetPipelineCacheData(device, pipelineCache, &dataSize, data.data());
    if (res != VK_SUCCESS) {
        LOGE("vkGetPipelineCacheData failed: %d", res);
        return;
    }

    std::string path = getPipelineCacheFilePath();
    if (path.empty()) {
        return;
    }

    std::string tmpPath = path + ".tmp";
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOGW("Failed to open temporary pipeline cache file %s for writing", tmpPath.c_str());
        return;
    }

    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(dataSize));
    out.close();

    if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
        LOGW("Failed to rename temporary pipeline cache to %s", path.c_str());
    } else {
        LOGI("Persisted Vulkan pipeline cache to %s (%.2f KB)", path.c_str(), dataSize / 1024.0f);
        pipelineCacheDirty = false;
    }
}

void VulkanRenderer::onTrimMemory(int level) {
    LOGI("VulkanRenderer::onTrimMemory(level=%d)", level);

    flushAsync();

    // Save pipeline cache on memory pressure or when UI is hidden
    savePipelineCache();

    // Android ComponentCallbacks2 levels:
    // TRIM_MEMORY_RUNNING_LOW (10), TRIM_MEMORY_RUNNING_CRITICAL (15),
    // TRIM_MEMORY_UI_HIDDEN (20), TRIM_MEMORY_BACKGROUND (40),
    // TRIM_MEMORY_MODERATE (60), TRIM_MEMORY_COMPLETE (80)
    if (level >= 10) {
        // Reset cached descriptor bindings so stale image views / samplers are released
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            lastBoundImageView[i] = VK_NULL_HANDLE;
            lastBoundSampler[i] = VK_NULL_HANDLE;
            lastBoundImageLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        LOGI("VulkanRenderer descriptor binding cache cleared on trim memory level %d", level);
    }

    if (level >= 80 /* TRIM_MEMORY_COMPLETE */) {
        waitQueueIdleLocked();
    }

    // Log active memory budget & heap usage
    VulkanContext::getInstance().logMemoryBudget();
}

void VulkanRenderer::destroyPipelineCache() {
    VkDevice device = VulkanContext::getInstance().getDevice();
    if (device != VK_NULL_HANDLE && pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device, pipelineCache, nullptr);
        pipelineCache = VK_NULL_HANDLE;
    }
    pipelineCacheDirty = false;
}

void VulkanRenderer::destroyRenderFinishedSemaphores() {
    VkDevice device = VulkanContext::getInstance().getDevice();
    if (device == VK_NULL_HANDLE) {
        renderFinishedSemaphores.clear();
        return;
    }
    for (VkSemaphore semaphore : renderFinishedSemaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
    }
    renderFinishedSemaphores.clear();
}

void VulkanRenderer::ensureRenderFinishedSemaphores(uint32_t count) {
    if (renderFinishedSemaphores.size() == count) {
        return;
    }
    destroyRenderFinishedSemaphores();
    VkDevice device = VulkanContext::getInstance().getDevice();
    if (device == VK_NULL_HANDLE || count == 0) {
        return;
    }
    renderFinishedSemaphores.assign(count, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sci {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < count; ++i) {
        if (vkCreateSemaphore(device, &sci, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS) {
            LOGE("vkCreateSemaphore failed for swapchain image %u", i);
            renderFinishedSemaphores[i] = VK_NULL_HANDLE;
        }
    }
}

void VulkanRenderer::deinitialize() {
    LOGI("Deinitializing VulkanRenderer...");
    stopAsyncWorker();
    VkDevice device = VulkanContext::getInstance().getDevice();
    if (device != VK_NULL_HANDLE) {
        waitQueueIdleLocked();

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (imageAvailableSemaphores[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
                imageAvailableSemaphores[i] = VK_NULL_HANDLE;
            }
        }
        destroyRenderFinishedSemaphores();

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (commandPools[i] != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, commandPools[i], nullptr);
                commandPools[i] = VK_NULL_HANDLE;
                commandBuffers[i] = VK_NULL_HANDLE;
            }
        }

        if (samplerLinear != VK_NULL_HANDLE) {
            vkDestroySampler(device, samplerLinear, nullptr);
            samplerLinear = VK_NULL_HANDLE;
        }

        if (samplerNearest != VK_NULL_HANDLE) {
            vkDestroySampler(device, samplerNearest, nullptr);
            samplerNearest = VK_NULL_HANDLE;
        }

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            lastBoundImageView[i] = VK_NULL_HANDLE;
            lastBoundSampler[i] = VK_NULL_HANDLE;
            lastBoundImageLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        }

        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }

        savePipelineCache();
        destroyPipelineCache();

        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }

        if (pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }

        if (descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
            descriptorSetLayout = VK_NULL_HANDLE;
        }

        if (renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }

        cleanupTimestampQueries();

        destroySwapchain();
    }

    initialized = false;
}

bool VulkanRenderer::surfaceMismatch() const {
    VkPhysicalDevice gpu = VulkanContext::getInstance().getGpu();
    VkSurfaceKHR surface = VulkanContext::getInstance().getSurface();
    if (gpu == VK_NULL_HANDLE || surface == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
        return false;
    }
    VkSurfaceCapabilitiesKHR caps {};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) != VK_SUCCESS) {
        return false;
    }
    if (caps.currentTransform != currentTransform) {
        return true;
    }
    if (caps.currentExtent.width != 0xFFFFFFFF &&
        (caps.currentExtent.width != swapchainExtent.width ||
         caps.currentExtent.height != swapchainExtent.height)) {
        return true;
    }
    return false;
}

void VulkanRenderer::updateScreenSize(uint32_t width, uint32_t height) {
    LOGI("VulkanRenderer::updateScreenSize %u x %u", width, height);
    const bool sizeChanged = screenWidth != width || screenHeight != height;
    if (sizeChanged) {
        screenWidth = width;
        screenHeight = height;
        pushConstantsDirty = true;
        isDirtyFlag = true;
    }
    if (initialized && (sizeChanged || surfaceMismatch())) {
        flushAsync();
        waitQueueIdleLocked();
        destroySwapchain();
        createSwapchain();
    }
}

void VulkanRenderer::updateAspectRatio(float aspectRatio) {
    if (aspectRatio > 0.0f && currentAspectRatio != aspectRatio) {
        currentAspectRatio = aspectRatio;
        pushConstantsDirty = true;
        isDirtyFlag = true;
    }
}

void VulkanRenderer::updateViewport(Rect viewport) {
    currentViewport = viewport;
    pushConstantsDirty = true;
    isDirtyFlag = true;
}

void VulkanRenderer::updateViewportAlignment(unsigned int alignment) {
    if (viewportAlignment != alignment) {
        viewportAlignment = alignment;
        pushConstantsDirty = true;
        isDirtyFlag = true;
    }
}

std::pair<float, float> VulkanRenderer::getRelativePosition(float touchX, float touchY) {
    float displayW = static_cast<float>(screenWidth > 0 ? screenWidth : (swapchainExtent.width > 0 ? swapchainExtent.width : 1));
    float displayH = static_cast<float>(screenHeight > 0 ? screenHeight : (swapchainExtent.height > 0 ? swapchainExtent.height : 1));
    if (screenWidth == 0 || screenHeight == 0) {
        if (currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
            displayW = static_cast<float>(swapchainExtent.height > 0 ? swapchainExtent.height : 1);
            displayH = static_cast<float>(swapchainExtent.width > 0 ? swapchainExtent.width : 1);
        }
    }

    float targetAspect = currentAspectRatio > 0.0f ? currentAspectRatio : (4.0f / 3.0f);

    float vpW = displayW * currentViewport.getWidth();
    float vpH = displayH * currentViewport.getHeight();
    float vpAspect = vpW / (vpH > 0.0f ? vpH : 1.0f);

    float scaleX = currentViewport.getWidth();
    float scaleY = currentViewport.getHeight();

    if (targetAspect > vpAspect) {
        scaleY *= (vpAspect / targetAspect);
    } else {
        scaleX *= (targetAspect / vpAspect);
    }

    float logicalOffsetX = (currentViewport.getX() * 2.0f) - (1.0f - currentViewport.getWidth());
    float logicalOffsetY = (currentViewport.getY() * 2.0f) - (1.0f - currentViewport.getHeight());

    float emptyVerticalSpace = currentViewport.getHeight() - scaleY;
    if (viewportAlignment == 1 /* V_ALIGN_TOP */) {
        logicalOffsetY -= emptyVerticalSpace;
    } else if (viewportAlignment == 2 /* V_ALIGN_BOTTOM */) {
        logicalOffsetY += emptyVerticalSpace;
    }

    float xMin = logicalOffsetX - scaleX;
    float xMax = logicalOffsetX + scaleX;
    float yMin = logicalOffsetY - scaleY;
    float yMax = logicalOffsetY + scaleY;

    if (xMax <= xMin || yMax <= yMin || touchX < xMin || touchX > xMax || touchY < yMin || touchY > yMax) {
        return {-10.0f, -10.0f};
    }

    float relativeX = std::clamp((touchX - xMin) / (xMax - xMin), 0.0f, 1.0f);
    float relativeY = std::clamp((touchY - yMin) / (yMax - yMin), 0.0f, 1.0f);

    LOGD("Vulkan computed relative touch position: %.2f, %.2f", relativeX, relativeY);
    return {relativeX, relativeY};
}

bool VulkanRenderer::createSwapchain() {
    VkPhysicalDevice gpu = VulkanContext::getInstance().getGpu();
    VkDevice device = VulkanContext::getInstance().getDevice();
    VkSurfaceKHR surface = VulkanContext::getInstance().getSurface();
    if (gpu == VK_NULL_HANDLE || device == VK_NULL_HANDLE || surface == VK_NULL_HANDLE) {
        return false;
    }

    VkSurfaceCapabilitiesKHR caps;
    VkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
    if (res != VK_SUCCESS) {
        LOGE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: %d", res);
        return false;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, formats.data());

    swapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool foundFormat = false;
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swapchainFormat = f.format;
            colorSpace = f.colorSpace;
            foundFormat = true;
            break;
        }
    }
    if (!foundFormat && !formats.empty()) {
        swapchainFormat = formats[0].format;
        colorSpace = formats[0].colorSpace;
    }

    currentTransform = caps.currentTransform;

    if (caps.currentExtent.width != 0xFFFFFFFF && caps.currentExtent.width > 0) {
        swapchainExtent = caps.currentExtent;
        // currentExtent is the window in its current orientation. Aspect and
        // pixel fit have to use this, not a size cached from the previous rotation.
        screenWidth = swapchainExtent.width;
        screenHeight = swapchainExtent.height;
    } else {
        uint32_t w = screenWidth > 0 ? screenWidth : 1920;
        uint32_t h = screenHeight > 0 ? screenHeight : 1080;
        if (currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
            std::swap(w, h);
        }
        swapchainExtent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        swapchainExtent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    LOGI("Creating swapchain: extent %u x %u, transform: %d, images: %u, format: %d",
         swapchainExtent.width, swapchainExtent.height, (int)currentTransform, imageCount, swapchainFormat);

    VkSwapchainCreateInfoKHR sci {};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = swapchainFormat;
    sci.imageColorSpace = colorSpace;
    sci.imageExtent = swapchainExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = currentTransform;
    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &presentModeCount, presentModes.data());

    for (auto pm : presentModes) {
        LOGI("Supported swapchain present mode: %d (%s)", pm,
             pm == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" :
             pm == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
             pm == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" :
             pm == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" : "OTHER");
    }

    bool hasMailbox = false;
    bool hasFifoRelaxed = false;
    bool hasImmediate = false;
    for (auto pm : presentModes) {
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) hasMailbox = true;
        if (pm == VK_PRESENT_MODE_FIFO_RELAXED_KHR) hasFifoRelaxed = true;
        if (pm == VK_PRESENT_MODE_IMMEDIATE_KHR) hasImmediate = true;
    }

    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (allowMailbox) {
        if (hasMailbox) {
            chosenPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (hasFifoRelaxed) {
            chosenPresentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        } else if (hasImmediate) {
            chosenPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    } else {
        // Under standard pacing, FIFO_RELAXED prevents momentary frame-time variance
        // from halving perceived framerate from 60 FPS down to 30 FPS.
        if (hasFifoRelaxed) {
            chosenPresentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        }
    }

    usingMailbox = (chosenPresentMode == VK_PRESENT_MODE_MAILBOX_KHR);
    LOGI("Selected swapchain present mode: %d (%s, allowMailbox=%d)",
         chosenPresentMode,
         chosenPresentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
         chosenPresentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" :
         chosenPresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "FIFO",
         allowMailbox);

    sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) {
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    sci.presentMode = chosenPresentMode;
    sci.clipped = VK_TRUE;

    res = vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed: %d", res);
        if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
            LOGE("CRITICAL: Out of memory creating swapchain");
            VulkanContext::getInstance().logMemoryBudget();
        }
        return false;
    }

    uint32_t actualImageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &actualImageCount, nullptr);
    swapchainImages.resize(actualImageCount);
    vkGetSwapchainImagesKHR(device, swapchain, &actualImageCount, swapchainImages.data());

    swapchainImageViews.resize(actualImageCount);
    for (size_t i = 0; i < actualImageCount; ++i) {
        VkImageViewCreateInfo ivci {};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = swapchainImages[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swapchainFormat;
        ivci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.baseMipLevel = 0;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount = 1;

        res = vkCreateImageView(device, &ivci, nullptr, &swapchainImageViews[i]);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateImageView failed for swapchain image %zu: %d", i, res);
            return false;
        }
    }

    if (!dynamicRenderingSupported && renderPass != VK_NULL_HANDLE) {
        swapchainFramebuffers.resize(actualImageCount);
        for (size_t i = 0; i < actualImageCount; ++i) {
            VkImageView attachments[] = { swapchainImageViews[i] };
            VkFramebufferCreateInfo fci {};
            fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fci.renderPass = renderPass;
            fci.attachmentCount = 1;
            fci.pAttachments = attachments;
            fci.width = swapchainExtent.width;
            fci.height = swapchainExtent.height;
            fci.layers = 1;

            res = vkCreateFramebuffer(device, &fci, nullptr, &swapchainFramebuffers[i]);
            if (res != VK_SUCCESS) {
                LOGE("vkCreateFramebuffer failed for swapchain framebuffer %zu: %d", i, res);
                return false;
            }
        }
    }

    ensureRenderFinishedSemaphores(actualImageCount);
    pushConstantsDirty = true;
    return true;
}

void VulkanRenderer::destroySwapchain() {
    flushAsync();
    VkDevice device = VulkanContext::getInstance().getDevice();
    if (device != VK_NULL_HANDLE) {
        for (auto fb : swapchainFramebuffers) {
            if (fb != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, fb, nullptr);
            }
        }
        swapchainFramebuffers.clear();

        for (auto iv : swapchainImageViews) {
            if (iv != VK_NULL_HANDLE) {
                vkDestroyImageView(device, iv, nullptr);
            }
        }
        swapchainImageViews.clear();
        swapchainImages.clear();

        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            lastBoundImageView[i] = VK_NULL_HANDLE;
            lastBoundSampler[i] = VK_NULL_HANDLE;
            lastBoundImageLayout[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }
}

bool VulkanRenderer::createRenderPass() {
    VkDevice device = VulkanContext::getInstance().getDevice();

    VkAttachmentDescription colorAttachment {};
    colorAttachment.format = swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;

    VkResult res = vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateRenderPass failed: %d", res);
        return false;
    }
    return true;
}

bool VulkanRenderer::createPipeline() {
    VkDevice device = VulkanContext::getInstance().getDevice();

    VkDescriptorSetLayoutBinding binding {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo {};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings = &binding;
    if (pushDescriptorSupported) {
        dslInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    }

    VkResult res = vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &descriptorSetLayout);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateDescriptorSetLayout failed: %d", res);
        return false;
    }

    VkPushConstantRange pcr {};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo plInfo {};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &descriptorSetLayout;
    plInfo.pushConstantRangeCount = 1;
    plInfo.pPushConstantRanges = &pcr;

    res = vkCreatePipelineLayout(device, &plInfo, nullptr, &pipelineLayout);
    if (res != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout failed: %d", res);
        return false;
    }

    VkShaderModuleCreateInfo vertSmInfo {};
    vertSmInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vertSmInfo.codeSize = quad_vert_spv_size;
    vertSmInfo.pCode = quad_vert_spv;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    res = vkCreateShaderModule(device, &vertSmInfo, nullptr, &vertModule);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateShaderModule (vert) failed: %d", res);
        return false;
    }

    VkShaderModuleCreateInfo fragSmInfo {};
    fragSmInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fragSmInfo.codeSize = quad_frag_spv_size;
    fragSmInfo.pCode = quad_frag_spv;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    res = vkCreateShaderModule(device, &fragSmInfo, nullptr, &fragModule);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateShaderModule (frag) failed: %d", res);
        vkDestroyShaderModule(device, vertModule, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertModule;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragModule;
    shaderStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInputInfo {};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment {};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending {};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    VkPipelineRenderingCreateInfoKHR renderingCreateInfo {};
    if (dynamicRenderingSupported) {
        renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        renderingCreateInfo.colorAttachmentCount = 1;
        renderingCreateInfo.pColorAttachmentFormats = &swapchainFormat;
        pipelineInfo.renderPass = VK_NULL_HANDLE;
        pipelineInfo.pNext = &renderingCreateInfo;
    } else {
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
    }

    auto compileStart = std::chrono::steady_clock::now();
    res = vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineInfo, nullptr, &pipeline);
    auto compileEnd = std::chrono::steady_clock::now();
    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);

    if (res != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed: %d", res);
        if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
            LOGE("CRITICAL: Out of memory creating graphics pipeline");
            VulkanContext::getInstance().logMemoryBudget();
        }
        return false;
    }
    pipelineCacheDirty = true;
    double compileMs = std::chrono::duration<double, std::milli>(compileEnd - compileStart).count();
    LRDStats::notePipelineCompile(compileMs);
    LOGI("Compiled Vulkan graphics pipeline in %.2f ms", compileMs);
    return true;
}

bool VulkanRenderer::createDescriptorObjects() {
    VkDevice device = VulkanContext::getInstance().getDevice();

    VkSamplerCreateInfo samplerLinearInfo {};
    samplerLinearInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerLinearInfo.magFilter = VK_FILTER_LINEAR;
    samplerLinearInfo.minFilter = VK_FILTER_LINEAR;
    samplerLinearInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerLinearInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerLinearInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerLinearInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VkResult res = vkCreateSampler(device, &samplerLinearInfo, nullptr, &samplerLinear);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateSampler (linear) failed: %d", res);
        return false;
    }

    VkSamplerCreateInfo samplerNearestInfo {};
    samplerNearestInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerNearestInfo.magFilter = VK_FILTER_NEAREST;
    samplerNearestInfo.minFilter = VK_FILTER_NEAREST;
    samplerNearestInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerNearestInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerNearestInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerNearestInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    res = vkCreateSampler(device, &samplerNearestInfo, nullptr, &samplerNearest);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateSampler (nearest) failed: %d", res);
        return false;
    }

    if (pushDescriptorSupported) {
        LOGI("Push descriptors supported: bypassing descriptor pool and set allocation");
        return true;
    }

    VkDescriptorPoolSize poolSize {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT * 4;

    VkDescriptorPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT * 4;

    res = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateDescriptorPool failed: %d", res);
        if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
            LOGE("CRITICAL: Out of memory creating descriptor pool");
            VulkanContext::getInstance().logMemoryBudget();
        }
        return false;
    }

    descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();

    res = vkAllocateDescriptorSets(device, &allocInfo, descriptorSets.data());
    if (res != VK_SUCCESS) {
        LOGE("vkAllocateDescriptorSets failed: %d", res);
        if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
            LOGE("CRITICAL: Out of memory allocating descriptor sets");
            VulkanContext::getInstance().logMemoryBudget();
        }
        return false;
    }

    return true;
}

bool VulkanRenderer::createCommandBuffers() {
    VkDevice device = VulkanContext::getInstance().getDevice();
    uint32_t queueFamilyIndex = VulkanContext::getInstance().getQueueFamilyIndex();

    VkCommandPoolCreateInfo cpInfo {};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpInfo.queueFamilyIndex = queueFamilyIndex;
    cpInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkResult res = vkCreateCommandPool(device, &cpInfo, nullptr, &commandPools[i]);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateCommandPool failed for frame %u: %d", i, res);
            return false;
        }

        VkCommandBufferAllocateInfo cbai {};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = commandPools[i];
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;

        res = vkAllocateCommandBuffers(device, &cbai, &commandBuffers[i]);
        if (res != VK_SUCCESS) {
            LOGE("vkAllocateCommandBuffers failed for frame %u: %d", i, res);
            return false;
        }
    }

    return true;
}

bool VulkanRenderer::createSyncObjects() {
    VkDevice device = VulkanContext::getInstance().getDevice();

    VkSemaphoreCreateInfo sci {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(device, &sci, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS) {
            LOGE("vkCreateSemaphore failed for frame %u", i);
            return false;
        }
    }
    return true;
}

void VulkanRenderer::recomputePushConstants() {
    // Calculate display dimensions in user logical space
    float displayW = static_cast<float>(screenWidth > 0 ? screenWidth : swapchainExtent.width);
    float displayH = static_cast<float>(screenHeight > 0 ? screenHeight : swapchainExtent.height);
    if (screenWidth == 0 || screenHeight == 0) {
        if (currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
            displayW = static_cast<float>(swapchainExtent.height);
            displayH = static_cast<float>(swapchainExtent.width);
        }
    }

    float targetAspect = currentAspectRatio > 0.0f ? currentAspectRatio : (4.0f / 3.0f);

    float vpW = displayW * currentViewport.getWidth();
    float vpH = displayH * currentViewport.getHeight();
    float vpAspect = vpW / (vpH > 0.0f ? vpH : 1.0f);

    float scaleX = currentViewport.getWidth();
    float scaleY = currentViewport.getHeight();

    if (targetAspect > vpAspect) {
        // Content is wider than viewport: letterbox vertically
        scaleY *= (vpAspect / targetAspect);
    } else {
        // Content is taller than viewport: pillarbox horizontally
        scaleX *= (targetAspect / vpAspect);
    }

    float logicalOffsetX = (currentViewport.getX() * 2.0f) - (1.0f - currentViewport.getWidth());
    float logicalOffsetY = (currentViewport.getY() * 2.0f) - (1.0f - currentViewport.getHeight());

    float emptyVerticalSpace = currentViewport.getHeight() - scaleY;
    if (viewportAlignment == 1 /* V_ALIGN_TOP */) {
        logicalOffsetY -= emptyVerticalSpace;
    } else if (viewportAlignment == 2 /* V_ALIGN_BOTTOM */) {
        logicalOffsetY += emptyVerticalSpace;
    }

    float cosTheta = 1.0f;
    float sinTheta = 0.0f;

    switch (currentTransform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
            cosTheta = 0.0f;
            sinTheta = 1.0f;
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
            cosTheta = -1.0f;
            sinTheta = 0.0f;
            break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
            cosTheta = 0.0f;
            sinTheta = -1.0f;
            break;
        case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:
        default:
            cosTheta = 1.0f;
            sinTheta = 0.0f;
            break;
    }

    // Matrix column 0 = (m00, m10), column 1 = (m01, m11)
    cachedPush.transform[0] = scaleX * cosTheta;
    cachedPush.transform[1] = scaleX * sinTheta;
    cachedPush.transform[2] = -scaleY * sinTheta;
    cachedPush.transform[3] = scaleY * cosTheta;

    cachedPush.offset[0] = logicalOffsetX * cosTheta - logicalOffsetY * sinTheta;
    cachedPush.offset[1] = logicalOffsetX * sinTheta + logicalOffsetY * cosTheta;

    isLetterboxed = (scaleX < 0.999f || scaleY < 0.999f);
}

bool VulkanRenderer::renderFrame() {
    isDirtyFlag = false;
    if (!initialized) {
        if (!initialize()) {
            return false;
        }
    }

    VkDevice device = VulkanContext::getInstance().getDevice();
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
        return false;
    }

    // Recreate only after acquire/present reports the surface changed, or after
    // a failed submit. Querying surface caps every frame races BufferQueue.
    if (needsRecreate) {
        waitQueueIdleLocked();
        destroySwapchain();
        if (!createSwapchain()) {
            return false;
        }
        needsRecreate = false;
    }

    // Wait on this slot's fence. Resetting is deferred until submission to avoid deadlock if acquire fails.
    VulkanContext::getInstance().waitForSyncFence();

    // Collect GPU timestamp query results from this slot's prior execution
    if (timestampsSupported && timestampQueryPool != VK_NULL_HANDLE && queryInFlight[currentFrame]) {
        uint64_t timestamps[2] = { 0, 0 };
        VkResult queryRes = vkGetQueryPoolResults(
            device,
            timestampQueryPool,
            currentFrame * 2,
            2,
            sizeof(timestamps),
            timestamps,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT
        );
        if (queryRes == VK_SUCCESS) {
            uint64_t mask = (timestampValidBits == 64) ? ~0ULL : ((1ULL << timestampValidBits) - 1ULL);
            uint64_t delta = (timestamps[1] - timestamps[0]) & mask;
            lastGpuTimeMs = static_cast<double>(delta) * static_cast<double>(timestampPeriod) / 1e6;
        }
        queryInFlight[currentFrame] = false;
    }

    uint32_t imageIndex = 0;
    VkResult res = vkAcquireNextImageKHR(
        device,
        swapchain,
        UINT64_MAX,
        imageAvailableSemaphores[currentFrame],
        VK_NULL_HANDLE,
        &imageIndex
    );

    const bool suboptimal = res == VK_SUBOPTIMAL_KHR;
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_ERROR_SURFACE_LOST_KHR) {
        // The acquire did not signal a semaphore. Recreate before the next frame.
        needsRecreate = true;
        return false;
    }
    if (res == VK_ERROR_DEVICE_LOST) {
        LOGE("CRITICAL: Vulkan Device Lost (VK_ERROR_DEVICE_LOST) in vkAcquireNextImageKHR");
        return false;
    }
    if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
        LOGE("CRITICAL: Vulkan Out of Memory (%d) in vkAcquireNextImageKHR", res);
        VulkanContext::getInstance().logMemoryBudget();
        return false;
    }
    if (res != VK_SUCCESS && !suboptimal) {
        LOGE("vkAcquireNextImageKHR failed: %d", res);
        return false;
    }
    if (imageIndex >= renderFinishedSemaphores.size() ||
        renderFinishedSemaphores[imageIndex] == VK_NULL_HANDLE) {
        LOGE("Missing present semaphore for swapchain image %u", imageIndex);
        return false;
    }

    const struct retro_vulkan_image* currentImage = VulkanContext::getInstance().getCurrentImage();
    VkSampler activeSampler = useLinearFiltering ? samplerLinear : samplerNearest;
    VkImageLayout targetLayout = (currentImage != nullptr && currentImage->image_layout != VK_IMAGE_LAYOUT_UNDEFINED)
                                 ? currentImage->image_layout
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (currentImage != nullptr && currentImage->image_view != VK_NULL_HANDLE) {
        // Fallback descriptor set update when VK_KHR_push_descriptor is not available
        if (!pushDescriptorSupported || pfnCmdPushDescriptorSetKHR == nullptr) {
            if (lastBoundImageView[currentFrame] != currentImage->image_view ||
                lastBoundSampler[currentFrame] != activeSampler ||
                lastBoundImageLayout[currentFrame] != targetLayout) {

                VkDescriptorImageInfo imageInfo {};
                imageInfo.sampler = activeSampler;
                imageInfo.imageView = currentImage->image_view;
                imageInfo.imageLayout = targetLayout;

                VkWriteDescriptorSet write {};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = descriptorSets[currentFrame];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imageInfo;

                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

                lastBoundImageView[currentFrame] = currentImage->image_view;
                lastBoundSampler[currentFrame] = activeSampler;
                lastBoundImageLayout[currentFrame] = targetLayout;
            }
        }
    }

    if (pushConstantsDirty) {
        recomputePushConstants();
        pushConstantsDirty = false;
    }

    VkCommandBuffer cmd = commandBuffers[currentFrame];
    vkResetCommandPool(device, commandPools[currentFrame], 0);

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    if (timestampsSupported && timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, timestampQueryPool, currentFrame * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampQueryPool, currentFrame * 2);
    }

    // Libretro Vulkan spec: ensure image layout is transitioned to SHADER_READ_ONLY_OPTIMAL
    // This activates Qualcomm Adreno UBWC (Universal Bandwidth Compression) hardware decompression
    if (currentImage != nullptr && currentImage->image_view != VK_NULL_HANDLE) {
        if (currentImage->create_info.image != VK_NULL_HANDLE &&
            currentImage->image_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            VkImageMemoryBarrier barrier {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = currentImage->image_layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = currentImage->create_info.image;
            barrier.subresourceRange = currentImage->create_info.subresourceRange;
            if (barrier.subresourceRange.aspectMask == 0) {
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
            }
            barrier.srcAccessMask = (currentImage->image_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                                    ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier
            );
            targetLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    if (dynamicRenderingSupported && pfnCmdBeginRendering != nullptr) {
        // Modern VK_KHR_dynamic_rendering path: bypasses VkRenderPass and VkFramebuffer allocations
        VkImageMemoryBarrier attachBarrier {};
        attachBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        attachBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        attachBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        attachBarrier.image = swapchainImages[imageIndex];
        attachBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachBarrier.subresourceRange.baseMipLevel = 0;
        attachBarrier.subresourceRange.levelCount = 1;
        attachBarrier.subresourceRange.baseArrayLayer = 0;
        attachBarrier.subresourceRange.layerCount = 1;
        attachBarrier.srcAccessMask = 0;
        attachBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &attachBarrier
        );

        VkRenderingAttachmentInfoKHR colorAttachment {};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
        colorAttachment.imageView = swapchainImageViews[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // TBDR optimization: DONT_CARE bypasses clearing on-chip GMEM tile memory on full-bleed quads
        colorAttachment.loadOp = isLetterboxed ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = { {{ 0.0f, 0.0f, 0.0f, 1.0f }} };

        VkRenderingInfoKHR renderingInfo {};
        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
        renderingInfo.renderArea.offset = { 0, 0 };
        renderingInfo.renderArea.extent = swapchainExtent;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        pfnCmdBeginRendering(cmd, &renderingInfo);

        if (currentImage != nullptr && currentImage->image_view != VK_NULL_HANDLE) {
            VkViewport vp { 0.0f, 0.0f, static_cast<float>(swapchainExtent.width), static_cast<float>(swapchainExtent.height), 0.0f, 1.0f };
            VkRect2D sc { { 0, 0 }, swapchainExtent };
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            if (pushDescriptorSupported && pfnCmdPushDescriptorSetKHR != nullptr) {
                VkDescriptorImageInfo imageInfo {};
                imageInfo.sampler = activeSampler;
                imageInfo.imageView = currentImage->image_view;
                imageInfo.imageLayout = targetLayout;

                VkWriteDescriptorSet write {};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = VK_NULL_HANDLE;
                write.dstBinding = 0;
                write.dstArrayElement = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imageInfo;

                pfnCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &write);
            } else {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets[currentFrame], 0, nullptr);
            }

            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &cachedPush);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }

        pfnCmdEndRendering(cmd);

        // Transition swapchain image from COLOR_ATTACHMENT_OPTIMAL to PRESENT_SRC_KHR
        VkImageMemoryBarrier presentBarrier {};
        presentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        presentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        presentBarrier.image = swapchainImages[imageIndex];
        presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        presentBarrier.subresourceRange.baseMipLevel = 0;
        presentBarrier.subresourceRange.levelCount = 1;
        presentBarrier.subresourceRange.baseArrayLayer = 0;
        presentBarrier.subresourceRange.layerCount = 1;
        presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        presentBarrier.dstAccessMask = 0;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &presentBarrier
        );
    } else {
        // Fallback VkRenderPassBeginInfo path
        VkClearValue clearColor = { {{ 0.0f, 0.0f, 0.0f, 1.0f }} };
        VkRenderPassBeginInfo rpbi {};
        rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass = renderPass;
        rpbi.framebuffer = swapchainFramebuffers[imageIndex];
        rpbi.renderArea.offset = { 0, 0 };
        rpbi.renderArea.extent = swapchainExtent;
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &clearColor;

        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        if (currentImage != nullptr && currentImage->image_view != VK_NULL_HANDLE) {
            VkViewport vp { 0.0f, 0.0f, static_cast<float>(swapchainExtent.width), static_cast<float>(swapchainExtent.height), 0.0f, 1.0f };
            VkRect2D sc { { 0, 0 }, swapchainExtent };
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            if (pushDescriptorSupported && pfnCmdPushDescriptorSetKHR != nullptr) {
                VkDescriptorImageInfo imageInfo {};
                imageInfo.sampler = activeSampler;
                imageInfo.imageView = currentImage->image_view;
                imageInfo.imageLayout = targetLayout;

                VkWriteDescriptorSet write {};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = VK_NULL_HANDLE;
                write.dstBinding = 0;
                write.dstArrayElement = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imageInfo;

                pfnCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &write);
            } else {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets[currentFrame], 0, nullptr);
            }

            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &cachedPush);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
    }

    if (timestampsSupported && timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampQueryPool, currentFrame * 2 + 1);
        queryInFlight[currentFrame] = true;
    }

    vkEndCommandBuffer(cmd);

    // Build wait and signal semaphores using fixed-size stack arrays (zero per-frame heap allocations)
    constexpr size_t MAX_SUBMIT_SEMAPHORES = 16;
    constexpr size_t MAX_SUBMIT_CMD_BUFFERS = 16;

    VkSemaphore waitSems[MAX_SUBMIT_SEMAPHORES];
    VkPipelineStageFlags waitStages[MAX_SUBMIT_SEMAPHORES];
    uint32_t waitCount = 0;

    waitSems[waitCount] = imageAvailableSemaphores[currentFrame];
    waitStages[waitCount] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    waitCount++;

    const auto& contextWaitSems = VulkanContext::getInstance().getWaitSemaphores();
    for (auto sem : contextWaitSems) {
        if (sem != VK_NULL_HANDLE && waitCount < MAX_SUBMIT_SEMAPHORES) {
            waitSems[waitCount] = sem;
            waitStages[waitCount] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            waitCount++;
        }
    }

    VkSemaphore signalSems[MAX_SUBMIT_SEMAPHORES];
    uint32_t signalCount = 0;
    signalSems[signalCount++] = renderFinishedSemaphores[imageIndex];

    VkSemaphore coreSignal = VulkanContext::getInstance().getSignalSemaphore();
    if (coreSignal != VK_NULL_HANDLE && signalCount < MAX_SUBMIT_SEMAPHORES) {
        signalSems[signalCount++] = coreSignal;
    }

    VkCommandBuffer allCmdBuffers[MAX_SUBMIT_CMD_BUFFERS];
    uint32_t cmdCount = 0;
    const auto& contextCmds = VulkanContext::getInstance().getCoreCommandBuffers();
    for (auto coreCmd : contextCmds) {
        if (coreCmd != VK_NULL_HANDLE && cmdCount < (MAX_SUBMIT_CMD_BUFFERS - 1)) {
            allCmdBuffers[cmdCount++] = coreCmd;
        }
    }
    allCmdBuffers[cmdCount++] = cmd;

    VkFence currentSyncFence = VulkanContext::getInstance().getCurrentSyncFence();
    VkQueue queue = VulkanContext::getInstance().getQueue();

    if (synchronization2Supported && pfnQueueSubmit2 != nullptr) {
        VkSemaphoreSubmitInfoKHR waitInfos[MAX_SUBMIT_SEMAPHORES];
        for (uint32_t i = 0; i < waitCount; ++i) {
            waitInfos[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            waitInfos[i].pNext = nullptr;
            waitInfos[i].semaphore = waitSems[i];
            waitInfos[i].value = 0;
            waitInfos[i].stageMask = (i == 0) ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT_KHR;
            waitInfos[i].deviceIndex = 0;
        }

        VkSemaphoreSubmitInfoKHR signalInfos[MAX_SUBMIT_SEMAPHORES];
        for (uint32_t i = 0; i < signalCount; ++i) {
            signalInfos[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR;
            signalInfos[i].pNext = nullptr;
            signalInfos[i].semaphore = signalSems[i];
            signalInfos[i].value = 0;
            signalInfos[i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR;
            signalInfos[i].deviceIndex = 0;
        }

        VkCommandBufferSubmitInfoKHR cmdInfos[MAX_SUBMIT_CMD_BUFFERS];
        for (uint32_t i = 0; i < cmdCount; ++i) {
            cmdInfos[i].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR;
            cmdInfos[i].pNext = nullptr;
            cmdInfos[i].commandBuffer = allCmdBuffers[i];
            cmdInfos[i].deviceMask = 0;
        }

        VkSubmitInfo2KHR si2 {};
        si2.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR;
        si2.waitSemaphoreInfoCount = waitCount;
        si2.pWaitSemaphoreInfos = waitInfos;
        si2.commandBufferInfoCount = cmdCount;
        si2.pCommandBufferInfos = cmdInfos;
        si2.signalSemaphoreInfoCount = signalCount;
        si2.pSignalSemaphoreInfos = signalInfos;

        {
            std::lock_guard<std::mutex> lock(VulkanContext::getInstance().getQueueMutex());
            VulkanContext::getInstance().resetSyncFence();
            res = pfnQueueSubmit2(queue, 1, &si2, currentSyncFence);
            if (res != VK_SUCCESS) {
                LOGE("vkQueueSubmit2KHR failed: %d", res);
                if (res == VK_ERROR_DEVICE_LOST) {
                    LOGE("CRITICAL: Vulkan Device Lost (VK_ERROR_DEVICE_LOST) in vkQueueSubmit2KHR");
                } else if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
                    LOGE("CRITICAL: Vulkan Out of Memory (%d) in vkQueueSubmit2KHR", res);
                    VulkanContext::getInstance().logMemoryBudget();
                } else {
                    needsRecreate = true;
                    vkQueueWaitIdle(queue);
                    if (imageAvailableSemaphores[currentFrame] != VK_NULL_HANDLE) {
                        vkDestroySemaphore(device, imageAvailableSemaphores[currentFrame], nullptr);
                        imageAvailableSemaphores[currentFrame] = VK_NULL_HANDLE;
                    }
                    VkSemaphoreCreateInfo replacement {};
                    replacement.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                    vkCreateSemaphore(device, &replacement, nullptr, &imageAvailableSemaphores[currentFrame]);
                }
                return false;
            }
            VulkanContext::getInstance().setCurrentFenceInFlight();
        }
    } else {
        VkSubmitInfo si {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = waitCount;
        si.pWaitSemaphores = waitSems;
        si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = cmdCount;
        si.pCommandBuffers = allCmdBuffers;
        si.signalSemaphoreCount = signalCount;
        si.pSignalSemaphores = signalSems;

        {
            std::lock_guard<std::mutex> lock(VulkanContext::getInstance().getQueueMutex());
            VulkanContext::getInstance().resetSyncFence();
            res = vkQueueSubmit(queue, 1, &si, currentSyncFence);
            if (res != VK_SUCCESS) {
                LOGE("vkQueueSubmit failed: %d", res);
                if (res == VK_ERROR_DEVICE_LOST) {
                    LOGE("CRITICAL: Vulkan Device Lost (VK_ERROR_DEVICE_LOST) in vkQueueSubmit");
                } else if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
                    LOGE("CRITICAL: Vulkan Out of Memory (%d) in vkQueueSubmit", res);
                    VulkanContext::getInstance().logMemoryBudget();
                } else {
                    needsRecreate = true;
                    vkQueueWaitIdle(queue);
                    if (imageAvailableSemaphores[currentFrame] != VK_NULL_HANDLE) {
                        vkDestroySemaphore(device, imageAvailableSemaphores[currentFrame], nullptr);
                        imageAvailableSemaphores[currentFrame] = VK_NULL_HANDLE;
                    }
                    VkSemaphoreCreateInfo replacement {};
                    replacement.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                    vkCreateSemaphore(device, &replacement, nullptr, &imageAvailableSemaphores[currentFrame]);
                }
                return false;
            }
            VulkanContext::getInstance().setCurrentFenceInFlight();
        }
    }
    // queueMutex is released here! Core threads (e.g. Dolphin/Aether/Cemu MTGS worker threads) can submit immediately.

    VkPresentInfoKHR pi {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinishedSemaphores[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;

    VkQueue presentQueue = VulkanContext::getInstance().getPresentationQueue();
    if (presentQueue == VK_NULL_HANDLE) {
        presentQueue = queue;
    }

    // Present the swapchain image to the display.
    // Note: queueMutex is intentionally NOT locked during vkQueuePresentKHR.
    // On Android, vkQueuePresentKHR interacts with SurfaceFlinger / BufferQueue and can block
    // waiting for VSYNC in FIFO mode. Holding queueMutex here would freeze core rendering threads
    // (such as PS2 MTGS or Cemu Latte) from calling vkQueueSubmit for up to 16.6ms.
    res = vkQueuePresentKHR(presentQueue, &pi);

    if (suboptimal || res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR || res == VK_ERROR_SURFACE_LOST_KHR) {
        needsRecreate = true;
    } else if (res == VK_ERROR_DEVICE_LOST) {
        LOGE("CRITICAL: Vulkan Device Lost (VK_ERROR_DEVICE_LOST) in vkQueuePresentKHR");
    } else if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
        LOGE("CRITICAL: Vulkan Out of Memory (%d) in vkQueuePresentKHR", res);
        VulkanContext::getInstance().logMemoryBudget();
    }

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    VulkanContext::getInstance().clearPerFrameState();
    VulkanContext::getInstance().advanceSyncIndex();

    return true;
}

void VulkanRenderer::initTimestampQueries() {
    VkPhysicalDevice gpu = VulkanContext::getInstance().getGpu();
    VkDevice device = VulkanContext::getInstance().getDevice();
    uint32_t queueFamilyIndex = VulkanContext::getInstance().getQueueFamilyIndex();
    if (gpu == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
        return;
    }

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(gpu, &props);
    timestampPeriod = props.limits.timestampPeriod;

    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProps(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueCount, queueProps.data());

    if (queueFamilyIndex < queueCount) {
        timestampValidBits = queueProps[queueFamilyIndex].timestampValidBits;
    }

    timestampsSupported = (timestampValidBits > 0) && (timestampPeriod > 0.0f);
    if (timestampsSupported) {
        VkQueryPoolCreateInfo qpci {};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = MAX_FRAMES_IN_FLIGHT * 2;
        VkResult res = vkCreateQueryPool(device, &qpci, nullptr, &timestampQueryPool);
        if (res != VK_SUCCESS) {
            LOGW("Failed to create timestamp query pool: %d", res);
            timestampsSupported = false;
            timestampQueryPool = VK_NULL_HANDLE;
        } else {
            LOGI("Vulkan timestamp queries enabled (period: %.2f ns, valid bits: %u)",
                 timestampPeriod, timestampValidBits);
        }
    } else {
        LOGI("Vulkan timestamp queries not supported by device/queue (valid bits: %u, period: %.2f)",
             timestampValidBits, timestampPeriod);
    }
}

void VulkanRenderer::cleanupTimestampQueries() {
    VkDevice device = VulkanContext::getInstance().getDevice();
    if (device != VK_NULL_HANDLE && timestampQueryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, timestampQueryPool, nullptr);
        timestampQueryPool = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        queryInFlight[i] = false;
    }
    lastGpuTimeMs = -1.0;
    timestampsSupported = false;
}

void VulkanRenderer::setAsyncPresentation(bool enabled) {
    if (asyncPresentation == enabled) return;
    flushAsync();
    asyncPresentation = enabled;
}

void VulkanRenderer::startAsyncWorker() {
    if (asyncWorkerRunning) return;
    asyncWorkerRunning = true;
    asyncFramePending = false;
    asyncFrameInFlight = false;
    asyncWorkerThread = std::thread(&VulkanRenderer::asyncWorkerLoop, this);
}

void VulkanRenderer::stopAsyncWorker() {
    if (!asyncWorkerRunning) return;
    {
        std::lock_guard<std::mutex> lock(asyncMutex);
        asyncWorkerRunning = false;
        asyncFramePending = false;
        asyncCv.notify_all();
    }
    if (asyncWorkerThread.joinable()) {
        asyncWorkerThread.join();
    }
}

void VulkanRenderer::asyncWorkerLoop() {
    pthread_setname_np(pthread_self(), "VulkanPresent");
    ADPFManager::getInstance().registerThread(gettid());

    while (asyncWorkerRunning) {
        {
            std::unique_lock<std::mutex> lock(asyncMutex);
            asyncCv.wait(lock, [this]() {
                return !asyncWorkerRunning || asyncFramePending.load();
            });
            if (!asyncWorkerRunning) break;
            asyncFramePending = false;
            asyncFrameInFlight = true;
        }

        renderFrame();

        {
            std::lock_guard<std::mutex> lock(asyncMutex);
            asyncFrameInFlight = false;
            asyncDoneCv.notify_all();
        }
    }

    ADPFManager::getInstance().unregisterThread(gettid());
}

void VulkanRenderer::requestRenderAsync() {
    if (!asyncPresentation || !asyncWorkerRunning) {
        renderFrame();
        return;
    }

    {
        std::unique_lock<std::mutex> lock(asyncMutex);
        // Wait for prior frame presentation to finish so we do not overwrite or race on per-frame buffers
        asyncDoneCv.wait(lock, [this]() {
            return !asyncFrameInFlight.load() && !asyncFramePending.load();
        });

        asyncFramePending = true;
        asyncCv.notify_one();
    }
}

void VulkanRenderer::flushAsync() {
    if (!asyncWorkerRunning) return;
    std::unique_lock<std::mutex> lock(asyncMutex);
    asyncDoneCv.wait(lock, [this]() {
        return !asyncFrameInFlight.load() && !asyncFramePending.load();
    });
}

} // namespace libretrodroid

