#include "vulkancontext.h"
#include <cstring>
#include <algorithm>

namespace libretrodroid {

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
    deinitialize();
}

bool VulkanContext::initialize(bool preferVulkan) {
    vulkanRequested = preferVulkan;
    return true;
}

void VulkanContext::deinitialize() {
    LOGI("Deinitializing VulkanContext...");
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        for (uint32_t i = 0; i < MAX_SYNC_INDEX; ++i) {
            if (syncFences[i] != VK_NULL_HANDLE) {
                vkDestroyFence(device, syncFences[i], nullptr);
                syncFences[i] = VK_NULL_HANDLE;
            }
        }

        if (negotiationInterface.has_value() && negotiationInterface->destroy_device != nullptr) {
            negotiationInterface->destroy_device();
        }

        if (!coreCreatedDevice && device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }
        device = VK_NULL_HANDLE;
    }

    destroySurface();

    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }

    gpu = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    queueFamilyIndex = 0;
    initialized = false;
    coreCreatedDevice = false;
    negotiationInterface.reset();
    contextReset = nullptr;
    contextDestroy = nullptr;
    currentImage = nullptr;
    hasLastImage = false;
    waitSemaphores.clear();
    coreCommandBuffers.clear();
    signalSemaphore = VK_NULL_HANDLE;
    syncIndex = 0;
    for (uint32_t i = 0; i < MAX_SYNC_INDEX; ++i) {
        fenceWaited[i] = false;
    }
}

void VulkanContext::setNegotiationInterface(const struct retro_hw_render_context_negotiation_interface_vulkan& iface) {
    LOGI("Received Vulkan context negotiation interface (version %u)", iface.interface_version);
    negotiationInterface = iface;
}

bool VulkanContext::initHwRender(struct retro_hw_render_callback* cb) {
    LOGI("VulkanContext::initHwRender called");
    if (!vulkanRequested) {
        vulkanRequested = true;
    }

    contextReset = cb->context_reset;
    contextDestroy = cb->context_destroy;
    cb->get_current_framebuffer = nullptr;
    cb->get_proc_address = nullptr;

    if (instance == VK_NULL_HANDLE) {
        if (!createInstance()) {
            return false;
        }
    }

    if (gpu == VK_NULL_HANDLE) {
        if (!selectPhysicalDevice()) {
            return false;
        }
    }

    if (device == VK_NULL_HANDLE) {
        if (!createDevice()) {
            return false;
        }
    }

    initialized = true;
    return true;
}

const struct retro_hw_render_interface_vulkan* VulkanContext::getInterface() {
    libretroInterface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    libretroInterface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    libretroInterface.handle = this;
    libretroInterface.instance = instance;
    libretroInterface.gpu = gpu;
    libretroInterface.device = device;
    libretroInterface.get_device_proc_addr = &vkGetDeviceProcAddr;
    libretroInterface.get_instance_proc_addr = &vkGetInstanceProcAddr;
    libretroInterface.queue = queue;
    libretroInterface.queue_index = queueFamilyIndex;
    libretroInterface.set_image = &VulkanContext::callbackSetImage;
    libretroInterface.get_sync_index = &VulkanContext::callbackGetSyncIndex;
    libretroInterface.get_sync_index_mask = &VulkanContext::callbackGetSyncIndexMask;
    libretroInterface.set_command_buffers = &VulkanContext::callbackSetCommandBuffers;
    libretroInterface.wait_sync_index = &VulkanContext::callbackWaitSyncIndex;
    libretroInterface.lock_queue = &VulkanContext::callbackLockQueue;
    libretroInterface.unlock_queue = &VulkanContext::callbackUnlockQueue;
    libretroInterface.set_signal_semaphore = &VulkanContext::callbackSetSignalSemaphore;

    LOGI("Returning populated retro_hw_render_interface_vulkan (dev=%p, queue=%p, queue_idx=%u)",
         device, queue, queueFamilyIndex);
    return &libretroInterface;
}

bool VulkanContext::createInstance() {
    const VkApplicationInfo* appInfo = nullptr;
    VkApplicationInfo defaultAppInfo {};
    defaultAppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    defaultAppInfo.pApplicationName = "Omnidroid";
    defaultAppInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    defaultAppInfo.pEngineName = "LibretroDroid";
    defaultAppInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    defaultAppInfo.apiVersion = VK_API_VERSION_1_1;

    if (negotiationInterface.has_value() && negotiationInterface->get_application_info != nullptr) {
        appInfo = negotiationInterface->get_application_info();
    }
    if (appInfo == nullptr) {
        appInfo = &defaultAppInfo;
    }

    LOGI("Creating VkInstance with target API version %u.%u.%u",
         VK_VERSION_MAJOR(appInfo->apiVersion),
         VK_VERSION_MINOR(appInfo->apiVersion),
         VK_VERSION_PATCH(appInfo->apiVersion));

    if (negotiationInterface.has_value() &&
        negotiationInterface->interface_version >= 2 &&
        negotiationInterface->create_instance != nullptr) {
        LOGI("Calling core negotiation create_instance wrapper");
        instance = negotiationInterface->create_instance(
            &vkGetInstanceProcAddr,
            appInfo,
            &VulkanContext::createInstanceWrapper,
            this
        );
    } else {
        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
        };

        VkInstanceCreateInfo createInfo {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkResult res = vkCreateInstance(&createInfo, nullptr, &instance);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateInstance failed: %d", res);
            return false;
        }
        LOGI("Frontend created VkInstance: %p", instance);
    }

    if (instance != VK_NULL_HANDLE && nativeWindow != nullptr && surface == VK_NULL_HANDLE) {
        VkAndroidSurfaceCreateInfoKHR createInfo {};
        createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        createInfo.window = nativeWindow;
        VkResult res = vkCreateAndroidSurfaceKHR(instance, &createInfo, nullptr, &surface);
        if (res == VK_SUCCESS) {
            LOGI("Created VkSurfaceKHR from cached ANativeWindow: %p", (void*)(uintptr_t)surface);
        } else {
            LOGE("Failed to create VkSurfaceKHR from cached ANativeWindow: %d", res);
        }
    }

    return instance != VK_NULL_HANDLE;
}

VkInstance VulkanContext::createInstanceWrapper(void* opaque, const VkInstanceCreateInfo* create_info) {
    (void)opaque;
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i) {
        extensions.push_back(create_info->ppEnabledExtensionNames[i]);
    }

    auto addExt = [&](const char* name) {
        for (const auto& ext : extensions) {
            if (strcmp(ext, name) == 0) return;
        }
        extensions.push_back(name);
    };

    addExt(VK_KHR_SURFACE_EXTENSION_NAME);
    addExt(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);

    VkInstanceCreateInfo modified = *create_info;
    modified.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modified.ppEnabledExtensionNames = extensions.data();

    VkInstance inst = VK_NULL_HANDLE;
    VkResult res = vkCreateInstance(&modified, nullptr, &inst);
    if (res != VK_SUCCESS) {
        LOGE("createInstanceWrapper: vkCreateInstance failed: %d", res);
        return VK_NULL_HANDLE;
    }
    return inst;
}

bool VulkanContext::selectPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        LOGE("No Vulkan physical devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(instance, &count, gpus.data());

    for (const auto& candidate : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(candidate, &props);

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProps(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queueProps.data());

        for (uint32_t q = 0; q < queueCount; ++q) {
            if ((queueProps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (queueProps[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                gpu = candidate;
                queueFamilyIndex = q;
                LOGI("Selected Vulkan GPU: %s, queue family %u", props.deviceName, q);
                return true;
            }
        }
    }

    // Fallback to first GPU with graphics bit
    for (const auto& candidate : gpus) {
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProps(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queueProps.data());

        for (uint32_t q = 0; q < queueCount; ++q) {
            if (queueProps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                gpu = candidate;
                queueFamilyIndex = q;
                LOGI("Selected fallback Vulkan GPU (graphics only), queue family %u", q);
                return true;
            }
        }
    }

    LOGE("No suitable Vulkan queue family found");
    return false;
}

void VulkanContext::setNativeWindow(ANativeWindow* window) {
    if (nativeWindow == window && surface != VK_NULL_HANDLE) {
        return;
    }

    nativeWindow = window;
    destroySurface();

    if (instance != VK_NULL_HANDLE && nativeWindow != nullptr) {
        VkAndroidSurfaceCreateInfoKHR createInfo {};
        createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        createInfo.window = nativeWindow;

        VkResult res = vkCreateAndroidSurfaceKHR(instance, &createInfo, nullptr, &surface);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateAndroidSurfaceKHR failed: %d", res);
        } else {
            LOGI("Created VkSurfaceKHR from ANativeWindow: %p", (void*)(uintptr_t)surface);
        }
    }
}

void VulkanContext::destroySurface() {
    if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
        LOGI("Destroying VkSurfaceKHR: %p", (void*)(uintptr_t)surface);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
}

bool VulkanContext::createDevice() {
    if (negotiationInterface.has_value() &&
        negotiationInterface->interface_version >= 2 &&
        negotiationInterface->create_device2 != nullptr) {
        LOGI("Creating device via negotiation interface v2 create_device2");
        struct retro_vulkan_context coreContext {};
        bool success = negotiationInterface->create_device2(
            &coreContext,
            instance,
            gpu,
            surface,
            &vkGetInstanceProcAddr,
            &VulkanContext::createDeviceWrapper,
            this
        );

        if (success) {
            device = coreContext.device;
            gpu = coreContext.gpu;
            queue = coreContext.queue;
            queueFamilyIndex = coreContext.queue_family_index;
            coreCreatedDevice = true;
            LOGI("Device created by core via create_device2 (dev=%p, queue=%p)", device, queue);
        } else {
            LOGW("Core create_device2 returned false, falling back to frontend device creation");
        }
    } else if (negotiationInterface.has_value() &&
               negotiationInterface->create_device != nullptr) {
        LOGI("Creating device via negotiation interface v1 create_device");
        const char* exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        struct retro_vulkan_context coreContext {};
        bool success = negotiationInterface->create_device(
            &coreContext,
            instance,
            gpu,
            surface,
            &vkGetInstanceProcAddr,
            exts,
            1,
            nullptr,
            0,
            nullptr
        );

        if (success) {
            device = coreContext.device;
            gpu = coreContext.gpu;
            queue = coreContext.queue;
            queueFamilyIndex = coreContext.queue_family_index;
            coreCreatedDevice = true;
            LOGI("Device created by core via create_device (dev=%p, queue=%p)", device, queue);
        } else {
            LOGW("Core create_device returned false, falling back to frontend device creation");
        }
    }

    if (device == VK_NULL_HANDLE) {
        LOGI("Creating device directly in frontend");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci {};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = queueFamilyIndex;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo dci {};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExts;

        VkResult res = vkCreateDevice(gpu, &dci, nullptr, &device);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateDevice failed: %d", res);
            return false;
        }

        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
        coreCreatedDevice = false;
        LOGI("Frontend VkDevice created: %p, queue: %p", device, queue);
    }

    // Initialize sync fences for synchronization
    for (uint32_t i = 0; i < MAX_SYNC_INDEX; ++i) {
        VkFenceCreateInfo fci {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device, &fci, nullptr, &syncFences[i]);
        fenceWaited[i] = false;
    }

    return device != VK_NULL_HANDLE;
}

VkDevice VulkanContext::createDeviceWrapper(VkPhysicalDevice physicalDevice, void* opaque, const VkDeviceCreateInfo* create_info) {
    (void)opaque;
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i) {
        extensions.push_back(create_info->ppEnabledExtensionNames[i]);
    }

    auto addExt = [&](const char* name) {
        for (const auto& ext : extensions) {
            if (strcmp(ext, name) == 0) return;
        }
        extensions.push_back(name);
    };

    addExt(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    VkDeviceCreateInfo modified = *create_info;
    modified.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modified.ppEnabledExtensionNames = extensions.data();

    VkDevice dev = VK_NULL_HANDLE;
    VkResult res = vkCreateDevice(physicalDevice, &modified, nullptr, &dev);
    if (res != VK_SUCCESS) {
        LOGE("createDeviceWrapper: vkCreateDevice failed: %d", res);
        return VK_NULL_HANDLE;
    }
    return dev;
}

void VulkanContext::setImage(
    const struct retro_vulkan_image* img,
    uint32_t numSemaphores,
    const VkSemaphore* semaphores,
    uint32_t srcQueueFamily
) {
    if (img != nullptr) {
        lastImage = *img;
        hasLastImage = true;
        currentImage = &lastImage;
    } else {
        currentImage = nullptr;
    }
    waitSemaphores.clear();
    if (numSemaphores > 0 && semaphores != nullptr) {
        waitSemaphores.assign(semaphores, semaphores + numSemaphores);
    }
}

void VulkanContext::setCommandBuffers(uint32_t numCmd, const VkCommandBuffer* cmd) {
    coreCommandBuffers.clear();
    if (numCmd > 0 && cmd != nullptr) {
        coreCommandBuffers.assign(cmd, cmd + numCmd);
    }
}

void VulkanContext::waitSyncIndex() {
    // The core calls this before reusing a slot. Waiting is enough: the
    // frontend resets the fence when it reuses the slot. Resetting here too
    // races the frontend and leaves the fence unsignaled for the next submit.
    std::lock_guard<std::mutex> lock(syncMutex);
    const uint32_t index = waitedIndex;
    if (index >= MAX_SYNC_INDEX || fenceWaited[index] || syncFences[index] == VK_NULL_HANDLE) {
        return;
    }
    vkWaitForFences(device, 1, &syncFences[index], VK_TRUE, UINT64_MAX);
}

void VulkanContext::waitAndResetSyncFence() {
    std::lock_guard<std::mutex> lock(syncMutex);
    if (syncIndex >= MAX_SYNC_INDEX || fenceWaited[syncIndex] || syncFences[syncIndex] == VK_NULL_HANDLE) {
        return;
    }
    vkWaitForFences(device, 1, &syncFences[syncIndex], VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &syncFences[syncIndex]);
    fenceWaited[syncIndex] = true;
}

uint32_t VulkanContext::getSyncIndex() {
    std::lock_guard<std::mutex> lock(syncMutex);
    waitedIndex = syncIndex;
    return syncIndex;
}

VkFence VulkanContext::getCurrentSyncFence() {
    std::lock_guard<std::mutex> lock(syncMutex);
    return syncFences[syncIndex];
}

void VulkanContext::setCurrentFenceInFlight() {
    std::lock_guard<std::mutex> lock(syncMutex);
    if (syncIndex < MAX_SYNC_INDEX) {
        fenceWaited[syncIndex] = false;
    }
}

void VulkanContext::setSignalSemaphore(VkSemaphore sem) {
    signalSemaphore = sem;
}

void VulkanContext::clearPerFrameState() {
    currentImage = nullptr;
    waitSemaphores.clear();
    coreCommandBuffers.clear();
    signalSemaphore = VK_NULL_HANDLE;
}

void VulkanContext::advanceSyncIndex() {
    std::lock_guard<std::mutex> lock(syncMutex);
    syncIndex = (syncIndex + 1) % MAX_SYNC_INDEX;
}

// Static Libretro callbacks
void VulkanContext::callbackSetImage(
    void* handle,
    const struct retro_vulkan_image* image,
    uint32_t num_semaphores,
    const VkSemaphore* semaphores,
    uint32_t src_queue_family
) {
    static_cast<VulkanContext*>(handle)->setImage(image, num_semaphores, semaphores, src_queue_family);
}

uint32_t VulkanContext::callbackGetSyncIndex(void* handle) {
    return static_cast<VulkanContext*>(handle)->getSyncIndex();
}

uint32_t VulkanContext::callbackGetSyncIndexMask(void* handle) {
    return static_cast<VulkanContext*>(handle)->getSyncIndexMask();
}

void VulkanContext::callbackSetCommandBuffers(void* handle, uint32_t num_cmd, const VkCommandBuffer* cmd) {
    static_cast<VulkanContext*>(handle)->setCommandBuffers(num_cmd, cmd);
}

void VulkanContext::callbackWaitSyncIndex(void* handle) {
    static_cast<VulkanContext*>(handle)->waitSyncIndex();
}

void VulkanContext::callbackLockQueue(void* handle) {
    static_cast<VulkanContext*>(handle)->queueMutex.lock();
}

void VulkanContext::callbackUnlockQueue(void* handle) {
    static_cast<VulkanContext*>(handle)->queueMutex.unlock();
}

void VulkanContext::callbackSetSignalSemaphore(void* handle, VkSemaphore semaphore) {
    static_cast<VulkanContext*>(handle)->setSignalSemaphore(semaphore);
}

} // namespace libretrodroid
