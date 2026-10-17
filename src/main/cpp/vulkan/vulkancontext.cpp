#include "vulkancontext.h"
#include <cstring>
#include <algorithm>
#include <dlfcn.h>

#ifndef VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
#define VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME "VK_KHR_get_physical_device_properties2"
#endif

#ifndef VK_EXT_MEMORY_BUDGET_EXTENSION_NAME
#define VK_EXT_MEMORY_BUDGET_EXTENSION_NAME "VK_EXT_memory_budget"
#endif

#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT static_cast<VkStructureType>(1000237000)
#endif

#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 static_cast<VkStructureType>(1000059006)
#endif

#ifndef VK_EXT_memory_budget
typedef struct VkPhysicalDeviceMemoryBudgetPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    VkDeviceSize heapBudget[VK_MAX_MEMORY_HEAPS];
    VkDeviceSize heapUsage[VK_MAX_MEMORY_HEAPS];
} VkPhysicalDeviceMemoryBudgetPropertiesEXT;
#endif

#ifndef VK_VERSION_1_1
#ifndef VK_KHR_get_physical_device_properties2
typedef struct VkPhysicalDeviceMemoryProperties2 {
    VkStructureType sType;
    void* pNext;
    VkPhysicalDeviceMemoryProperties memoryProperties;
} VkPhysicalDeviceMemoryProperties2;
typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceMemoryProperties2)(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties);
#endif
#endif

namespace libretrodroid {

VulkanContext::VulkanContext() {
    waitSemaphores.reserve(16);
    coreCommandBuffers.reserve(16);
}

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
    if (nativeWindow != nullptr) {
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
    }

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
    contextResetCalled = false;
    currentImage = nullptr;
    hasLastImage = false;
    waitSemaphores.clear();
    coreCommandBuffers.clear();
    signalSemaphore = VK_NULL_HANDLE;
    syncIndex = 0;
    for (uint32_t i = 0; i < MAX_SYNC_INDEX; ++i) {
        fenceWaited[i] = false;
    }
    memoryBudgetSupported = false;
    isUma = false;
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

    return true;
}

bool VulkanContext::ensureInitialized() {
    if (initialized) {
        return true;
    }

    LOGI("VulkanContext::ensureInitialized: initializing Vulkan instance, GPU, and device...");

    if (instance == VK_NULL_HANDLE) {
        if (!createInstance()) {
            LOGE("Failed to create Vulkan instance in ensureInitialized");
            return false;
        }
    }

    if (gpu == VK_NULL_HANDLE) {
        if (!selectPhysicalDevice()) {
            LOGE("Failed to select physical device in ensureInitialized");
            return false;
        }
    }

    if (device == VK_NULL_HANDLE) {
        if (!createDevice()) {
            LOGE("Failed to create Vulkan device in ensureInitialized");
            return false;
        }
    }

    initialized = true;
    LOGI("VulkanContext successfully initialized!");
    return true;
}

const struct retro_hw_render_interface_vulkan* VulkanContext::getInterface() {
    if (!initialized) {
        ensureInitialized();
    }
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
    uint32_t supportedApiVersion = VK_API_VERSION_1_0;
    auto pfnEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (pfnEnumerateInstanceVersion != nullptr) {
        if (pfnEnumerateInstanceVersion(&supportedApiVersion) != VK_SUCCESS) {
            supportedApiVersion = VK_API_VERSION_1_0;
        }
    }

    uint32_t targetApiVersion = VK_API_VERSION_1_0;
    if (supportedApiVersion >= VK_API_VERSION_1_3) {
        targetApiVersion = VK_API_VERSION_1_3;
    } else if (supportedApiVersion >= VK_API_VERSION_1_2) {
        targetApiVersion = VK_API_VERSION_1_2;
    } else if (supportedApiVersion >= VK_API_VERSION_1_1) {
        targetApiVersion = VK_API_VERSION_1_1;
    }

    const VkApplicationInfo* appInfo = nullptr;
    VkApplicationInfo defaultAppInfo {};
    defaultAppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    defaultAppInfo.pApplicationName = "Omnidroid";
    defaultAppInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    defaultAppInfo.pEngineName = "LibretroDroid";
    defaultAppInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    defaultAppInfo.apiVersion = targetApiVersion;

    if (negotiationInterface.has_value() && negotiationInterface->get_application_info != nullptr) {
        appInfo = negotiationInterface->get_application_info();
    }
    if (appInfo == nullptr) {
        appInfo = &defaultAppInfo;
    }

    LOGI("Creating VkInstance with target API version %u.%u.%u (driver supports up to %u.%u.%u)",
         VK_VERSION_MAJOR(appInfo->apiVersion),
         VK_VERSION_MINOR(appInfo->apiVersion),
         VK_VERSION_PATCH(appInfo->apiVersion),
         VK_VERSION_MAJOR(supportedApiVersion),
         VK_VERSION_MINOR(supportedApiVersion),
         VK_VERSION_PATCH(supportedApiVersion));

    std::vector<const char*> layers;
#ifndef NDEBUG
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    if (layerCount > 0) {
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
        for (const auto& layer : availableLayers) {
            if (strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                LOGI("Found and enabled Khronos Vulkan validation layer");
                break;
            }
        }
    }
#endif

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
        if (instance != VK_NULL_HANDLE) {
            LOGI("Core created VkInstance via create_instance: %p", instance);
        } else {
            LOGW("Core create_instance returned VK_NULL_HANDLE, falling back to frontend instance creation");
        }
    }

    if (instance == VK_NULL_HANDLE) {
        std::vector<const char*> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
        };

        uint32_t instExtCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr);
        if (instExtCount > 0) {
            std::vector<VkExtensionProperties> availableInstExts(instExtCount);
            vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, availableInstExts.data());
            for (const auto& ext : availableInstExts) {
                if (strcmp(ext.extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
                    extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
                    break;
                }
            }
        }

        VkInstanceCreateInfo createInfo {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

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

    uint32_t instExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, nullptr);
    if (instExtCount > 0) {
        std::vector<VkExtensionProperties> availableInstExts(instExtCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &instExtCount, availableInstExts.data());
        for (const auto& ext : availableInstExts) {
            if (strcmp(ext.extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
                addExt(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
                break;
            }
        }
    }

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

    // Score GPUs: discrete > integrated > virtual > cpu
    auto scoreGpu = [](VkPhysicalDevice device) -> int {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 1000;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 800;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 400;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: return 10;
            default: return 100;
        }
    };

    std::stable_sort(gpus.begin(), gpus.end(), [&](VkPhysicalDevice a, VkPhysicalDevice b) {
        return scoreGpu(a) > scoreGpu(b);
    });

    for (const auto& candidate : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(candidate, &props);

        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            LOGW("Encountered software Vulkan physical device: %s", props.deviceName);
        }

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProps(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queueProps.data());

        for (uint32_t q = 0; q < queueCount; ++q) {
            const bool hasGraphics = (queueProps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            const bool hasCompute = (queueProps[q].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;

            VkBool32 presentSupported = VK_FALSE;
            if (surface != VK_NULL_HANDLE) {
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, q, surface, &presentSupported);
            } else {
                presentSupported = VK_TRUE;
            }

            if (hasGraphics && hasCompute && presentSupported) {
                gpu = candidate;
                queueFamilyIndex = q;
                LOGI("Selected Vulkan GPU: %s (type %d), queue family %u (graphics+compute+present)",
                     props.deviceName, (int)props.deviceType, q);
                logMemoryProperties();
                return true;
            }
        }
    }

    // Fallback pass: any graphics queue with presentation
    for (const auto& candidate : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(candidate, &props);

        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProps(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queueProps.data());

        for (uint32_t q = 0; q < queueCount; ++q) {
            if (queueProps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                VkBool32 presentSupported = VK_FALSE;
                if (surface != VK_NULL_HANDLE) {
                    vkGetPhysicalDeviceSurfaceSupportKHR(candidate, q, surface, &presentSupported);
                } else {
                    presentSupported = VK_TRUE;
                }
                if (presentSupported) {
                    gpu = candidate;
                    queueFamilyIndex = q;
                    LOGI("Selected fallback Vulkan GPU: %s, queue family %u", props.deviceName, q);
                    logMemoryProperties();
                    return true;
                }
            }
        }
    }

    LOGE("No suitable Vulkan queue family found");
    return false;
}

void VulkanContext::setNativeWindow(ANativeWindow* window) {
    if (nativeWindow == window) {
        if (window != nullptr) {
            // Balance reference from ANativeWindow_fromSurface
            ANativeWindow_release(window);
        }
        return;
    }

    destroySurface();

    if (nativeWindow != nullptr) {
        ANativeWindow_release(nativeWindow);
        nativeWindow = nullptr;
    }

    nativeWindow = window;

    if (instance != VK_NULL_HANDLE && nativeWindow != nullptr) {
        VkAndroidSurfaceCreateInfoKHR createInfo {};
        createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        createInfo.window = nativeWindow;

        VkResult res = vkCreateAndroidSurfaceKHR(instance, &createInfo, nullptr, &surface);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateAndroidSurfaceKHR failed: %d", res);
        } else {
            LOGI("Created VkSurfaceKHR from ANativeWindow: %p", (void*)(uintptr_t)surface);

            // Verify surface presentation support on current queue family
            if (gpu != VK_NULL_HANDLE) {
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(gpu, queueFamilyIndex, surface, &supported);
                if (!supported) {
                    LOGW("Current queue family %u does NOT support presentation to new surface!", queueFamilyIndex);
                }
            }

            applyTargetFrameRate();
        }
    }
}

void VulkanContext::setTargetFrameRate(float fps) {
    targetFps = fps;
    applyTargetFrameRate();
}

void VulkanContext::applyTargetFrameRate() {
    if (nativeWindow == nullptr || targetFps <= 0.0f) {
        return;
    }
    typedef int32_t (*PFN_ANativeWindow_setFrameRateWithStrategy)(ANativeWindow*, float, int8_t, int8_t);
    typedef int32_t (*PFN_ANativeWindow_setFrameRate)(ANativeWindow*, float, int8_t);

    static auto pfnSetFrameRateWithStrategy = reinterpret_cast<PFN_ANativeWindow_setFrameRateWithStrategy>(
        dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRateWithChangeFrameRateStrategy")
    );
    static auto pfnSetFrameRate = reinterpret_cast<PFN_ANativeWindow_setFrameRate>(
        dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate")
    );

    if (pfnSetFrameRateWithStrategy != nullptr) {
        // 0 = ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT
        // 0 = ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS
        int32_t res = pfnSetFrameRateWithStrategy(nativeWindow, targetFps, 0, 0);
        LOGI("ANativeWindow_setFrameRateWithChangeFrameRateStrategy(%.2f) returned %d", targetFps, res);
    } else if (pfnSetFrameRate != nullptr) {
        int32_t res = pfnSetFrameRate(nativeWindow, targetFps, 0);
        LOGI("ANativeWindow_setFrameRate(%.2f) returned %d", targetFps, res);
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
            if (coreContext.gpu != VK_NULL_HANDLE) {
                gpu = coreContext.gpu;
            }
            queue = coreContext.queue;
            queueFamilyIndex = coreContext.queue_family_index;
            presentationQueue = (coreContext.presentation_queue != VK_NULL_HANDLE)
                ? coreContext.presentation_queue
                : coreContext.queue;
            presentationQueueFamilyIndex = (coreContext.presentation_queue != VK_NULL_HANDLE)
                ? coreContext.presentation_queue_family_index
                : coreContext.queue_family_index;
            coreCreatedDevice = true;
            LOGI("Device created by core via create_device2 (dev=%p, queue=%p, queue_idx=%u, present_queue=%p)",
                 device, queue, queueFamilyIndex, presentationQueue);
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
            if (coreContext.gpu != VK_NULL_HANDLE) {
                gpu = coreContext.gpu;
            }
            queue = coreContext.queue;
            queueFamilyIndex = coreContext.queue_family_index;
            presentationQueue = (coreContext.presentation_queue != VK_NULL_HANDLE)
                ? coreContext.presentation_queue
                : coreContext.queue;
            presentationQueueFamilyIndex = (coreContext.presentation_queue != VK_NULL_HANDLE)
                ? coreContext.presentation_queue_family_index
                : coreContext.queue_family_index;
            coreCreatedDevice = true;
            LOGI("Device created by core via create_device (dev=%p, queue=%p, queue_idx=%u, present_queue=%p)",
                 device, queue, queueFamilyIndex, presentationQueue);
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

        std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        if (memoryBudgetSupported) {
            devExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
        VkDeviceCreateInfo dci {};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
        dci.ppEnabledExtensionNames = devExts.data();

        VkResult res = vkCreateDevice(gpu, &dci, nullptr, &device);
        if (res != VK_SUCCESS) {
            LOGE("vkCreateDevice failed: %d", res);
            return false;
        }

        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
        presentationQueue = queue;
        presentationQueueFamilyIndex = queueFamilyIndex;
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
    auto* ctx = reinterpret_cast<VulkanContext*>(opaque);
    const bool budgetSupported = (ctx != nullptr) ? ctx->memoryBudgetSupported : VulkanContext::getInstance().memoryBudgetSupported;

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
    if (budgetSupported) {
        addExt(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }

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

void VulkanContext::logMemoryProperties() {
    if (gpu == VK_NULL_HANDLE) {
        return;
    }

    VkPhysicalDeviceMemoryProperties memProperties {};
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProperties);

    // Determine Unified Memory Architecture (UMA)
    // On mobile SoCs, device-local heaps are host-visible (or there is only a single shared heap).
    bool hasHostVisibleDeviceLocal = false;
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memProperties.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            hasHostVisibleDeviceLocal = true;
            break;
        }
    }
    isUma = hasHostVisibleDeviceLocal || (memProperties.memoryHeapCount == 1 &&
            (memProperties.memoryHeaps[0].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT));

    LOGI("Vulkan Memory Architecture: %s (heaps: %u, types: %u)",
         isUma ? "Unified Memory (UMA / Mobile SoC)" : "Discrete Memory",
         memProperties.memoryHeapCount,
         memProperties.memoryTypeCount);

    VkPhysicalDeviceProperties devProps {};
    vkGetPhysicalDeviceProperties(gpu, &devProps);
    LOGI("Vulkan Memory Limits: maxAllocations=%u, nonCoherentAtomSize=%llu B, minMapAlignment=%llu B",
         devProps.limits.maxMemoryAllocationCount,
         static_cast<unsigned long long>(devProps.limits.nonCoherentAtomSize),
         static_cast<unsigned long long>(devProps.limits.minMemoryMapAlignment));

    for (uint32_t i = 0; i < memProperties.memoryHeapCount; ++i) {
        uint64_t sizeMB = memProperties.memoryHeaps[i].size / (1024 * 1024);
        LOGI("  Heap %u: %llu MB (flags: 0x%08x%s%s)",
             i,
             static_cast<unsigned long long>(sizeMB),
             memProperties.memoryHeaps[i].flags,
             (memProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " [DEVICE_LOCAL]" : "",
             (memProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT) ? " [MULTI_INSTANCE]" : "");
    }

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        const auto& type = memProperties.memoryTypes[i];
        std::string flagsStr;
        if (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) flagsStr += "DEVICE_LOCAL ";
        if (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) flagsStr += "HOST_VISIBLE ";
        if (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) flagsStr += "HOST_COHERENT ";
        if (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) flagsStr += "HOST_CACHED ";
        if (type.propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) flagsStr += "LAZILY_ALLOCATED ";
        if (type.propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT) flagsStr += "PROTECTED ";

        LOGI("  Type %2u: heap %u, flags: 0x%08x (%s)",
             i, type.heapIndex, type.propertyFlags, flagsStr.c_str());
    }

    // Check for VK_EXT_memory_budget extension support
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, nullptr);
    if (extCount > 0) {
        std::vector<VkExtensionProperties> devExts(extCount);
        vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, devExts.data());
        for (const auto& ext : devExts) {
            if (strcmp(ext.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                memoryBudgetSupported = true;
                break;
            }
        }
    }
    LOGI("Vulkan VK_EXT_memory_budget supported: %s", memoryBudgetSupported ? "YES" : "NO");

    logMemoryBudget();
}

void VulkanContext::logMemoryBudget() {
    if (gpu == VK_NULL_HANDLE) {
        return;
    }

    auto pfnGetMemProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2")
    );
    if (pfnGetMemProps2 == nullptr) {
        pfnGetMemProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2KHR")
        );
    }

    if (memoryBudgetSupported && pfnGetMemProps2 != nullptr) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps {};
        budgetProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

        VkPhysicalDeviceMemoryProperties2 props2 {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        props2.pNext = &budgetProps;

        pfnGetMemProps2(gpu, &props2);

        LOGI("Vulkan Memory Budget Status (via VK_EXT_memory_budget):");
        for (uint32_t i = 0; i < props2.memoryProperties.memoryHeapCount; ++i) {
            uint64_t usageMB = budgetProps.heapUsage[i] / (1024 * 1024);
            uint64_t budgetMB = budgetProps.heapBudget[i] / (1024 * 1024);
            uint64_t totalMB = props2.memoryProperties.memoryHeaps[i].size / (1024 * 1024);
            LOGI("  Heap %u [%s]: Usage = %llu MB / Budget = %llu MB (Total = %llu MB)",
                 i,
                 (props2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL" : "HOST",
                 static_cast<unsigned long long>(usageMB),
                 static_cast<unsigned long long>(budgetMB),
                 static_cast<unsigned long long>(totalMB));
        }
    } else {
        VkPhysicalDeviceMemoryProperties memProps {};
        vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);
        LOGI("Vulkan Memory Heap Status:");
        for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
            uint64_t totalMB = memProps.memoryHeaps[i].size / (1024 * 1024);
            LOGI("  Heap %u [%s]: Total = %llu MB",
                 i,
                 (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL" : "HOST",
                 static_cast<unsigned long long>(totalMB));
        }
    }
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
        for (uint32_t i = 0; i < numSemaphores; ++i) {
            waitSemaphores.push_back(semaphores[i]);
        }
    }
}

void VulkanContext::setCommandBuffers(uint32_t numCmd, const VkCommandBuffer* cmd) {
    coreCommandBuffers.clear();
    if (numCmd > 0 && cmd != nullptr) {
        for (uint32_t i = 0; i < numCmd; ++i) {
            coreCommandBuffers.push_back(cmd[i]);
        }
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

void VulkanContext::waitForSyncFence() {
    std::lock_guard<std::mutex> lock(syncMutex);
    if (syncIndex >= MAX_SYNC_INDEX || fenceWaited[syncIndex] || syncFences[syncIndex] == VK_NULL_HANDLE) {
        return;
    }
    vkWaitForFences(device, 1, &syncFences[syncIndex], VK_TRUE, UINT64_MAX);
    fenceWaited[syncIndex] = true;
}

void VulkanContext::resetSyncFence() {
    std::lock_guard<std::mutex> lock(syncMutex);
    if (syncIndex < MAX_SYNC_INDEX && syncFences[syncIndex] != VK_NULL_HANDLE) {
        vkResetFences(device, 1, &syncFences[syncIndex]);
    }
}

void VulkanContext::waitAndResetSyncFence() {
    waitForSyncFence();
    resetSyncFence();
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
