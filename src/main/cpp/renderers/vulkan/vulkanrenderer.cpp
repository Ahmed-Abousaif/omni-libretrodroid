#include "vulkanrenderer.h"
#include <algorithm>
#include <cstring>
#include <chrono>
#include "../../stats.h"

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

    if (!createSwapchain()) {
        LOGE("Failed to create Vulkan swapchain");
        return false;
    }

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

    initialized = true;
    LOGI("VulkanRenderer successfully initialized!");
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
    if (initialized) {
        needsRecreate = true;
    }
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

        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool, nullptr);
            commandPool = VK_NULL_HANDLE;
        }

        if (sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, sampler, nullptr);
            sampler = VK_NULL_HANDLE;
        }

        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
        }

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
    }
    if (initialized && (sizeChanged || surfaceMismatch())) {
        waitQueueIdleLocked();
        destroySwapchain();
        createSwapchain();
    }
}

void VulkanRenderer::updateAspectRatio(float aspectRatio) {
    if (aspectRatio > 0.0f) {
        currentAspectRatio = aspectRatio;
    }
}

void VulkanRenderer::updateViewport(Rect viewport) {
    currentViewport = viewport;
}

void VulkanRenderer::updateViewportAlignment(unsigned int alignment) {
    viewportAlignment = alignment;
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

    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (allowMailbox) {
        for (auto pm : presentModes) {
            if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
                chosenPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                break;
            }
        }
    }
    usingMailbox = chosenPresentMode == VK_PRESENT_MODE_MAILBOX_KHR;

    sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) {
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    sci.presentMode = chosenPresentMode;
    sci.clipped = VK_TRUE;

    res = vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed: %d", res);
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

    if (renderPass != VK_NULL_HANDLE) {
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
    return true;
}

void VulkanRenderer::destroySwapchain() {
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
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    auto compileStart = std::chrono::steady_clock::now();
    res = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    auto compileEnd = std::chrono::steady_clock::now();
    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);

    if (res != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed: %d", res);
        return false;
    }
    double compileMs = std::chrono::duration<double, std::milli>(compileEnd - compileStart).count();
    LRDStats::notePipelineCompile(compileMs);
    LOGI("Compiled Vulkan graphics pipeline in %.2f ms", compileMs);
    return true;
}

bool VulkanRenderer::createDescriptorObjects() {
    VkDevice device = VulkanContext::getInstance().getDevice();

    VkSamplerCreateInfo samplerInfo {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    VkResult res = vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateSampler failed: %d", res);
        return false;
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
    cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult res = vkCreateCommandPool(device, &cpInfo, nullptr, &commandPool);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateCommandPool failed: %d", res);
        return false;
    }

    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo cbai {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    res = vkAllocateCommandBuffers(device, &cbai, commandBuffers.data());
    if (res != VK_SUCCESS) {
        LOGE("vkAllocateCommandBuffers failed: %d", res);
        return false;
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

bool VulkanRenderer::renderFrame() {
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

    // Wait on this slot's fence and reset it once. The core waits without resetting.
    VulkanContext::getInstance().waitAndResetSyncFence();

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
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        // The acquire did not signal a semaphore. Recreate before the next frame.
        needsRecreate = true;
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
    if (currentImage != nullptr && currentImage->image_view != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imageInfo {};
        imageInfo.sampler = sampler;
        imageInfo.imageView = currentImage->image_view;
        imageInfo.imageLayout = currentImage->image_layout != VK_IMAGE_LAYOUT_UNDEFINED
                                ? currentImage->image_layout
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSets[currentFrame];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

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

    PushConstants push {};
    // Matrix column 0 = (m00, m10), column 1 = (m01, m11)
    push.transform[0] = scaleX * cosTheta;
    push.transform[1] = scaleX * sinTheta;
    push.transform[2] = -scaleY * sinTheta;
    push.transform[3] = scaleY * cosTheta;

    push.offset[0] = logicalOffsetX * cosTheta - logicalOffsetY * sinTheta;
    push.offset[1] = logicalOffsetX * sinTheta + logicalOffsetY * cosTheta;

    VkCommandBuffer cmd = commandBuffers[currentFrame];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    if (timestampsSupported && timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, timestampQueryPool, currentFrame * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampQueryPool, currentFrame * 2);
    }

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
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets[currentFrame], 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &push);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd);

    if (timestampsSupported && timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestampQueryPool, currentFrame * 2 + 1);
        queryInFlight[currentFrame] = true;
    }

    vkEndCommandBuffer(cmd);

    // Build wait and signal semaphores
    std::vector<VkSemaphore> waitSems;
    std::vector<VkPipelineStageFlags> waitStages;

    waitSems.push_back(imageAvailableSemaphores[currentFrame]);
    waitStages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    for (auto sem : VulkanContext::getInstance().getWaitSemaphores()) {
        if (sem != VK_NULL_HANDLE) {
            waitSems.push_back(sem);
            waitStages.push_back(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
    }

    std::vector<VkSemaphore> signalSems;
    signalSems.push_back(renderFinishedSemaphores[imageIndex]);

    VkSemaphore coreSignal = VulkanContext::getInstance().getSignalSemaphore();
    if (coreSignal != VK_NULL_HANDLE) {
        signalSems.push_back(coreSignal);
    }

    std::vector<VkCommandBuffer> allCmdBuffers;
    for (auto coreCmd : VulkanContext::getInstance().getCoreCommandBuffers()) {
        if (coreCmd != VK_NULL_HANDLE) {
            allCmdBuffers.push_back(coreCmd);
        }
    }
    allCmdBuffers.push_back(cmd);

    VkSubmitInfo si {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = static_cast<uint32_t>(waitSems.size());
    si.pWaitSemaphores = waitSems.data();
    si.pWaitDstStageMask = waitStages.data();
    si.commandBufferCount = static_cast<uint32_t>(allCmdBuffers.size());
    si.pCommandBuffers = allCmdBuffers.data();
    si.signalSemaphoreCount = static_cast<uint32_t>(signalSems.size());
    si.pSignalSemaphores = signalSems.data();

    VkFence currentSyncFence = VulkanContext::getInstance().getCurrentSyncFence();
    VkQueue queue = VulkanContext::getInstance().getQueue();

    {
        std::lock_guard<std::mutex> lock(VulkanContext::getInstance().getQueueMutex());
        res = vkQueueSubmit(queue, 1, &si, currentSyncFence);
        if (res != VK_SUCCESS) {
            // Present would queue a buffer whose wait semaphore never signaled.
            // Android crashes in Surface::queueBuffer on that path.
            LOGE("vkQueueSubmit failed: %d", res);
            needsRecreate = true;
            vkQueueWaitIdle(queue);
            if (imageAvailableSemaphores[currentFrame] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, imageAvailableSemaphores[currentFrame], nullptr);
                imageAvailableSemaphores[currentFrame] = VK_NULL_HANDLE;
            }
            VkSemaphoreCreateInfo replacement {};
            replacement.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(device, &replacement, nullptr, &imageAvailableSemaphores[currentFrame]);
        } else {
            VulkanContext::getInstance().setCurrentFenceInFlight();

            VkPresentInfoKHR pi {};
            pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &renderFinishedSemaphores[imageIndex];
            pi.swapchainCount = 1;
            pi.pSwapchains = &swapchain;
            pi.pImageIndices = &imageIndex;

            res = vkQueuePresentKHR(queue, &pi);
            if (suboptimal || res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
                needsRecreate = true;
            }
        }
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

} // namespace libretrodroid

