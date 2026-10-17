/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <jni.h>
#include <sys/resource.h>

#include <EGL/egl.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>

#include "libretrodroid.h"
#include "utils/libretrodroidexception.h"
#include "log.h"
#include "core.h"
#include "audio.h"
#include "video.h"
#include "renderers/renderer.h"
#include "fpssync.h"
#include "stats.h"
#include "input.h"
#include "rumble.h"
#include "shadermanager.h"
#include "utils/javautils.h"
#include "environment.h"
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es2/imagerendereres2.h"
#include "renderers/es3/imagerendereres3.h"
#include "utils/utils.h"
#include "utils/rect.h"
#include "errorcodes.h"
#include "vfs/vfs.h"
#include "utils/adpf.h"

namespace libretrodroid {

uintptr_t LibretroDroid::callback_get_current_framebuffer() {
    return LibretroDroid::getInstance().handleGetCurrentFrameBuffer();
}

void LibretroDroid::callback_hw_video_refresh(
    const void *data,
    unsigned width,
    unsigned height,
    size_t pitch
) {
    LibretroDroid::getInstance().handleVideoRefresh(data, width, height, pitch);
}

void LibretroDroid::callback_audio_sample(int16_t left, int16_t right) {
    LOGE("callback audio sample (left, right) has been called");
}

size_t LibretroDroid::callback_set_audio_sample_batch(const int16_t *data, size_t frames) {
    return LibretroDroid::getInstance().handleAudioCallback(data, frames);
}

void LibretroDroid::callback_retro_set_input_poll() {
    // Do nothing in here...
}

int16_t LibretroDroid::callback_set_input_state(
    unsigned int port,
    unsigned int device,
    unsigned int index,
    unsigned int id
) {
    return LibretroDroid::getInstance().handleSetInputState(port, device, index, id);
}

void LibretroDroid::updateAudioSampleRateMultiplier() {
    if (audio) {
        audio->setPlaybackSpeed(frameSpeed);
    }
}

// TODO... Do we really need this?
void LibretroDroid::resetGlobalVariables() {
    core = nullptr;
    audio = nullptr;
    video = nullptr;
    vulkanRenderer = nullptr;
    fpsSync = nullptr;
    input = nullptr;
    rumble = nullptr;
    nonBlockingVulkanPresent = false;
}

int LibretroDroid::availableDisks() {
    return Environment::getInstance().getRetroDiskControlCallback() != nullptr
           ? Environment::getInstance().getRetroDiskControlCallback()->get_num_images()
           : 0;
}

int LibretroDroid::currentDisk() {
    return Environment::getInstance().getRetroDiskControlCallback() != nullptr
           ? Environment::getInstance().getRetroDiskControlCallback()->get_image_index()
           : 0;
}

void LibretroDroid::changeDisk(unsigned int index) {
    if (Environment::getInstance().getRetroDiskControlCallback() == nullptr) {
        LOGE("Cannot swap disk. This platform does not support it.");
        return;
    }

    if (index < 0 || index >= Environment::getInstance().getRetroDiskControlCallback()->get_num_images()) {
        LOGE("Requested image index is not valid.");
        return;
    }

    if (Environment::getInstance().getRetroDiskControlCallback()->get_image_index() != index) {
        Environment::getInstance().getRetroDiskControlCallback()->set_eject_state(true);
        Environment::getInstance().getRetroDiskControlCallback()->set_image_index((unsigned) index);
        Environment::getInstance().getRetroDiskControlCallback()->set_eject_state(false);
    }
}

void LibretroDroid::updateVariable(const Variable& variable) {
    Environment::getInstance().updateVariable(variable.key, variable.value);
}

std::vector<Variable> LibretroDroid::getVariables() {
    return Environment::getInstance().getVariables();
}

std::vector<std::vector<struct Controller>> LibretroDroid::getControllers() {
    return Environment::getInstance().getControllers();
}

void LibretroDroid::setControllerType(unsigned int port, unsigned int type) {
    core->retro_set_controller_port_device(port, type);
}

bool LibretroDroid::unserializeState(int8_t *data, size_t size) {
    std::lock_guard<std::mutex> lock(coreLock);

    if (data == nullptr || size == 0) {
        LOGE("unserializeState received null data or 0 size");
        return false;
    }

    bool result = core->retro_unserialize(data, size);
    LOGI("retro_unserialize size=%zu returned %s", size, result ? "true" : "false");
    return result;
}

JNIEXPORT jboolean JNICALL LibretroDroid::unserializeSRAM(int8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(coreLock);

    size_t sramSize = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (sramSize == 0 || size == 0) {
        return true;
    }
    void *sramState = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);

    if (sramState == nullptr) {
        LOGI("Cannot load SRAM: nullptr in retro_get_memory_data (core may not use SAVE_RAM)");
        return true;
    }

    if (size > sramSize) {
        LOGE("Cannot load SRAM: size mismatch (provided %zu, max %zu)", size, sramSize);
        return false;
    }

    memcpy(sramState, data, size);

    return true;
}

std::pair<int8_t*, size_t> LibretroDroid::serializeSRAM() {
    std::lock_guard<std::mutex> lock(coreLock);

    size_t size = core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (size == 0) {
        return std::pair(nullptr, 0);
    }
    void* sramData = core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (sramData == nullptr) {
        return std::pair(nullptr, 0);
    }

    auto* data = new int8_t[size];
    memcpy(data, (int8_t*) sramData, size);

    return std::pair(data, size);
}

void LibretroDroid::setSurface(ANativeWindow* window) {
    LOGI("LibretroDroid::setSurface %p", window);
    VulkanContext::getInstance().setNativeWindow(window);
}

void LibretroDroid::onSurfaceDestroyed() {
    LOGI("LibretroDroid::onSurfaceDestroyed");
    ADPFManager::getInstance().closeSession();
    if (vulkanRenderer) {
        vulkanRenderer->deinitialize();
        vulkanRenderer = nullptr;
    }
    VulkanContext::getInstance().destroySurface();
}

void LibretroDroid::onSurfaceChanged(unsigned int width, unsigned int height) {
    LOGD("Performing libretrodroid onSurfaceChanged %u x %u", width, height);
    if (vulkanRenderer) {
        vulkanRenderer->updateScreenSize(width, height);
    }
    if (video) {
        video->updateScreenSize(width, height);
    }
}

void LibretroDroid::onSurfaceCreated() {
    LOGD("Performing libretrodroid onSurfaceCreated");

    if (VulkanContext::getInstance().isVulkanRequested()) {
        LOGI("Initializing Vulkan rendering in onSurfaceCreated");
        if (!VulkanContext::getInstance().ensureInitialized()) {
            LOGE("Failed to ensure VulkanContext is initialized in onSurfaceCreated");
            return;
        }
        vulkanRenderer = std::make_unique<VulkanRenderer>();
        if (!cacheDirectory.empty()) {
            vulkanRenderer->setCacheDirectory(cacheDirectory);
        }
        vulkanRenderer->initialize();
        auto shaders = ShaderManager::getShader(fragmentShaderConfig);
        vulkanRenderer->setLinearFiltering(shaders.linearTexture);
        vulkanRenderer->updateAspectRatio(getAspectRatio());
        vulkanRenderer->updateViewport(viewportRect);
        vulkanRenderer->updateViewportAlignment(viewportAlignment);

        if (!VulkanContext::getInstance().isContextResetCalled()) {
            if (VulkanContext::getInstance().getContextReset() != nullptr) {
                LOGI("Calling Vulkan context_reset callback");
                VulkanContext::getInstance().getContextReset()();
                VulkanContext::getInstance().setContextResetCalled(true);
            }
        } else {
            LOGI("Vulkan context_reset already called, skipping on surface recreation");
        }
        return;
    }

    struct retro_system_av_info system_av_info {};
    core->retro_get_system_av_info(&system_av_info);

    video = nullptr;

    Video::RenderingOptions renderingOptions {
        Environment::getInstance().isUseHwAcceleration(),
        system_av_info.geometry.base_width,
        system_av_info.geometry.base_height,
        Environment::getInstance().isUseDepth(),
        Environment::getInstance().isUseStencil(),
        openglESVersion,
        Environment::getInstance().getPixelFormat()
    };

    auto newVideo = new Video(
        renderingOptions,
        fragmentShaderConfig,
        Environment::getInstance().isBottomLeftOrigin(),
        Environment::getInstance().getScreenRotation(),
        skipDuplicateFrames,
        immersiveModeEnabled,
        viewportRect,
        immersiveModeConfig,
        viewportAlignment
    );

    video = std::unique_ptr<Video>(newVideo);

    if (Environment::getInstance().getHwContextReset() != nullptr) {
        Environment::getInstance().getHwContextReset()();
    }

    if (video && Environment::getInstance().isUseHwAcceleration()) {
        glBindFramebuffer(GL_FRAMEBUFFER, video->getCurrentFramebuffer());
    }
}

void LibretroDroid::onMotionEvent(
    unsigned int port,
    unsigned int source,
    float xAxis,
    float yAxis
) {
    LOGD("Received motion event: %d %.2f, %.2f", source, xAxis, yAxis);
    if (input) {
        input->onMotionEvent(port, source, xAxis, yAxis);
    }
}

void LibretroDroid::onTouchEvent(float xAxis, float yAxis) {
    LOGD("Received touch event: %.2f, %.2f", xAxis, yAxis);
    if (!input) {
        return;
    }
    if (video) {
        auto [x, y] = video->getLayout().getRelativePosition(xAxis, yAxis);
        input->onMotionEvent(0, Input::MOTION_SOURCE_POINTER, x, y);
    } else if (vulkanRenderer) {
        auto [x, y] = vulkanRenderer->getRelativePosition(xAxis, yAxis);
        input->onMotionEvent(0, Input::MOTION_SOURCE_POINTER, x, y);
    }
}

void LibretroDroid::onKeyEvent(unsigned int port, int action, int keyCode) {
    LOGD("Received key event with action (%d) and keycode (%d)", action, keyCode);
    if (input) {
        input->onKeyEvent(port, action, keyCode);
    }
}

void LibretroDroid::create(
    unsigned int GLESVersion,
    const std::string& soFilePath,
    const std::string& systemDir,
    const std::string& savesDir,
    std::vector<Variable> variables,
    const ShaderManager::Config& shaderConfig,
    float refreshRate,
    bool lowLatencyAudio,
    bool enableVirtualFileSystem,
    bool enableMicrophone,
    bool duplicateFrames,
    bool frameCatchUp,
    std::optional<ImmersiveMode::Config> immersiveModeConfig,
    const std::string& language
) {
    LOGD("Performing libretrodroid create");

    resetGlobalVariables();

    Environment::getInstance().initialize(systemDir, savesDir, &callback_get_current_framebuffer);
    Environment::getInstance().setLanguage(language);
    Environment::getInstance().setEnableVirtualFileSystem(enableVirtualFileSystem);
    Environment::getInstance().setEnableMicrophone(enableMicrophone);

    if (GLESVersion == 5) {
        LOGI("LibretroDroid created in Vulkan mode");
        VulkanContext::getInstance().initialize(true);
    }

    openglESVersion = GLESVersion;
    screenRefreshRate = refreshRate;
    skipDuplicateFrames = duplicateFrames;
    allowFrameCatchUp = frameCatchUp;
    immersiveModeEnabled = GLESVersion >= 3 && GLESVersion != 5 && immersiveModeConfig.has_value();
    this->immersiveModeConfig = immersiveModeConfig.value_or(ImmersiveMode::Config{});
    audioEnabled = true;
    frameSpeed = 1;

    core = std::make_unique<Core>(soFilePath);

    core->retro_set_video_refresh(&callback_hw_video_refresh);
    core->retro_set_environment(&Environment::callback_environment);
    core->retro_set_audio_sample(&callback_audio_sample);
    core->retro_set_audio_sample_batch(&callback_set_audio_sample_batch);
    core->retro_set_input_poll(&callback_retro_set_input_poll);
    core->retro_set_input_state(&callback_set_input_state);

    std::for_each(variables.begin(), variables.end(), [&](const Variable& v) {
        updateVariable(v);
    });

    core->retro_init();

    preferLowLatencyAudio = lowLatencyAudio;

    // HW accelerated cores are only supported on opengles 3 or Vulkan.
    if (Environment::getInstance().isUseHwAcceleration() && openglESVersion < 3 && openglESVersion != 5) {
        throw LibretroDroidError("OpenGL ES 3 or Vulkan is required for this Core", ERROR_GL_NOT_COMPATIBLE);
    }

    fragmentShaderConfig = shaderConfig;

    rumble = std::make_unique<Rumble>();
}

void LibretroDroid::loadGameFromPath(const std::string& gamePath) {
    LOGD("Performing libretrodroid loadGameFromPath");
    if (!core) {
        LOGE("loadGameFromPath called but core is not initialized");
        throw std::runtime_error("Core is not initialized");
    }
    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    struct retro_game_info game_info {};
    game_info.path = Utils::cloneToCString(gamePath);
    game_info.meta = nullptr;

    if (system_info.need_fullpath) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        struct Utils::ReadResult file = Utils::readFileAsBytes(gamePath);
        game_info.data = file.data;
        game_info.size = file.size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::loadGameFromBytes(const int8_t *data, size_t size) {
    LOGD("Performing libretrodroid loadGameFromBytes");
    if (!core) {
        LOGE("loadGameFromBytes called but core is not initialized");
        throw std::runtime_error("Core is not initialized");
    }

    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    struct retro_game_info game_info {};
    game_info.path = nullptr;
    game_info.meta = nullptr;

    if (system_info.need_fullpath) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        game_info.data = data;
        game_info.size = size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::loadGameFromVirtualFiles(std::vector<VFSFile> virtualFiles) {
    LOGD("Performing libretrodroid loadGameFromVirtualFiles");
    if (!core) {
        LOGE("loadGameFromVirtualFiles called but core is not initialized");
        throw std::runtime_error("Core is not initialized");
    }
    struct retro_system_info system_info {};
    core->retro_get_system_info(&system_info);

    if (virtualFiles.empty()) {
        LOGE("Calling loadGameFromVirtualFiles without any file.");
        throw std::runtime_error("Calling loadGameFromVirtualFiles without any file.");
    }

    std::string firstFilePath = virtualFiles[0].getFileName();
    int firstFileFD = virtualFiles[0].getFD();

    bool loadUsingVFS = system_info.need_fullpath || virtualFiles.size() > 1;

    struct retro_game_info game_info {};
    game_info.path = Utils::cloneToCString(firstFilePath);
    game_info.meta = nullptr;

    if (loadUsingVFS) {
        VFS::getInstance().initialize(std::move(virtualFiles));
    }

    if (loadUsingVFS) {
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        struct Utils::ReadResult file = Utils::readFileAsBytes(firstFileFD);
        game_info.data = file.data;
        game_info.size = file.size;
    }

    bool result = core->retro_load_game(&game_info);
    if (!result) {
        LOGE("Cannot load game. Leaving.");
        throw std::runtime_error("Cannot load game");
    }

    afterGameLoad();
}

void LibretroDroid::destroy() {
    std::lock_guard<std::mutex> lock(coreLock);

    LOGD("Performing libretrodroid destroy");

    if (VulkanContext::getInstance().isVulkanRequested()) {
        if (VulkanContext::getInstance().getContextDestroy() != nullptr) {
            LOGI("Calling Vulkan context_destroy callback");
            VulkanContext::getInstance().getContextDestroy()();
        }
    } else if (Environment::getInstance().getHwContextDestroy() != nullptr) {
        Environment::getInstance().getHwContextDestroy()();
    }

    if (core) {
        core->retro_unload_game();
        core->retro_deinit();
    }

    if (vulkanRenderer) {
        vulkanRenderer->deinitialize();
        vulkanRenderer = nullptr;
    }
    if (VulkanContext::getInstance().isVulkanRequested()) {
        VulkanContext::getInstance().deinitialize();
    }

    video = nullptr;
    core = nullptr;
    rumble = nullptr;
    fpsSync = nullptr;
    audio = nullptr;

    Environment::getInstance().deinitialize();
    VFS::getInstance().deinitialize();
}

void LibretroDroid::resume() {
    LOGD("Performing libretrodroid resume");

    input = std::make_unique<Input>();

    fpsSync->reset();
    audio->start();
    refreshAspectRatio();
}

void LibretroDroid::pause() {
    LOGD("Performing libretrodroid pause");
    audio->stop();

    if (vulkanRenderer) {
        vulkanRenderer->savePipelineCache();
    }

    input = nullptr;
}

void LibretroDroid::step() {
    static thread_local bool s_prioritySet = false;
    if (!s_prioritySet) {
        setpriority(PRIO_PROCESS, 0, -16);
        s_prioritySet = true;
    }

    std::unique_lock<std::mutex> lock(coreLock);
    const auto stepStart = std::chrono::steady_clock::now();

    LOGD("Stepping into retro_run()");

    if (vulkanRenderer && fpsSync) {
        // FIFO present blocks inside the queue lock for a full refresh. A core
        // that submits from its own thread (PS2's CPU thread) then only gets
        // the queue between presents, and a heavy scene locks to half rate.
        // MAILBOX returns immediately; the frame wait below runs after the lock
        // is released, and only if MAILBOX was actually selected.
        vulkanRenderer->setAllowMailbox(!fpsSync->usesVSync() || nonBlockingVulkanPresent);
        fpsSync->setSoftwarePace(nonBlockingVulkanPresent && vulkanRenderer->isUsingMailbox());
    }

    unsigned frames = 1;
    if (fpsSync) {
        unsigned requestedFrames = fpsSync->advanceFrames();

        // Heavy cores already spend the whole slice inside one retro_run.
        // Running a second one when late only deepens the stall.
        frames = allowFrameCatchUp ? std::min(requestedFrames, 2u) : 1u;
    }

    const bool profiling = LRDStats::isProfilingEnabled();
    std::chrono::steady_clock::time_point runStart;
    if (profiling) {
        runStart = std::chrono::steady_clock::now();
    }

    for (size_t i = 0; i < frames * frameSpeed; i++)
        core->retro_run();

    std::chrono::steady_clock::time_point renderStart;
    double runMs = 0.0;
    if (profiling) {
        auto runEnd = std::chrono::steady_clock::now();
        runMs = std::chrono::duration<double, std::milli>(runEnd - runStart).count();
        renderStart = std::chrono::steady_clock::now();
    }

    double gpuTimeMs = -1.0;
    if (vulkanRenderer) {
        vulkanRenderer->renderFrame();
        if (profiling) {
            gpuTimeMs = vulkanRenderer->getLastGpuTimeMs();
        }
    } else if (video && !video->rendersInVideoCallback()) {
        video->renderFrame();
    }

    if (profiling) {
        const auto now = std::chrono::steady_clock::now();
        const double renderMs = std::chrono::duration<double, std::milli>(now - renderStart).count();
        static thread_local auto lastPresent = std::chrono::steady_clock::now();
        const double presentIntervalMs = std::chrono::duration<double, std::milli>(now - lastPresent).count();
        lastPresent = now;

        const double targetIntervalMs = screenRefreshRate > 0.0f ? (1000.0 / screenRefreshRate) : 16.67;
        LRDStats::noteFrame(runMs, renderMs, gpuTimeMs, presentIntervalMs, targetIntervalMs);
    }

    const auto frameWorkEnd = std::chrono::steady_clock::now();
    const int64_t workNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(frameWorkEnd - stepStart).count();
    if (workNanos > 0) {
        ADPFManager::getInstance().reportActualDuration(workNanos);
    }

    if (rumble && rumbleEnabled) {
        rumble->fetchFromEnvironment();
    }

    // Some games override the core geometry at runtime. These fields get updated in retro_run().
    if (Environment::getInstance().isGameGeometryUpdated()) {
        Environment::getInstance().clearGameGeometryUpdated();

        if (video) {
            video->updateRendererSize(
                Environment::getInstance().getGameGeometryWidth(),
                Environment::getInstance().getGameGeometryHeight()
            );
        }
        if (vulkanRenderer) {
            vulkanRenderer->updateAspectRatio(getAspectRatio());
        }

        dirtyVideo = true;
    }

    if (video && Environment::getInstance().isScreenRotationUpdated()) {
        Environment::getInstance().clearScreenRotationUpdated();

        video->updateRotation(Environment::getInstance().getScreenRotation());
    }

    // Release coreLock before blocking on fpsSync->wait() so UI/input threads are not stalled during VSync idle wait.
    lock.unlock();

    if (fpsSync) {
        fpsSync->wait();
    }
}

float LibretroDroid::getAspectRatio() {
    float gameAspectRatio = Environment::getInstance().retrieveGameSpecificAspectRatio();
    return gameAspectRatio > 0 ? gameAspectRatio : defaultAspectRatio;
}

void LibretroDroid::refreshAspectRatio() {
    if (video) {
        video->updateAspectRatio(getAspectRatio());
    }
    if (vulkanRenderer) {
        vulkanRenderer->updateAspectRatio(getAspectRatio());
    }
}

void LibretroDroid::setRumbleEnabled(bool enabled) {
    rumbleEnabled = enabled;
}

void LibretroDroid::setNonBlockingVulkanPresent(bool enabled) {
    nonBlockingVulkanPresent = enabled;
}

bool LibretroDroid::isRumbleEnabled() const {
    return rumbleEnabled;
}

void LibretroDroid::setFrameSpeed(unsigned int speed) {
    frameSpeed = speed;
    updateAudioSampleRateMultiplier();
}

void LibretroDroid::setAudioEnabled(bool enabled) {
    audioEnabled = enabled;
}

void LibretroDroid::setShaderConfig(ShaderManager::Config shaderConfig) {
    fragmentShaderConfig = std::move(shaderConfig);
    if (video) {
        video->updateShaderType(fragmentShaderConfig);
    }
    if (vulkanRenderer) {
        auto shaders = ShaderManager::getShader(fragmentShaderConfig);
        vulkanRenderer->setLinearFiltering(shaders.linearTexture);
    }
}

void LibretroDroid::setCacheDirectory(const std::string& cacheDir) {
    this->cacheDirectory = cacheDir;
    if (vulkanRenderer) {
        vulkanRenderer->setCacheDirectory(cacheDir);
    }
}

void LibretroDroid::onTrimMemory(int level) {
    LOGI("LibretroDroid::onTrimMemory level=%d", level);
    if (vulkanRenderer) {
        vulkanRenderer->onTrimMemory(level);
    }
}

void LibretroDroid::handleVideoRefresh(
    const void *data,
    unsigned int width,
    unsigned int height,
    size_t pitch
) {
    if (video) {
        video->onNewFrame(data, width, height, pitch);

        if (video->rendersInVideoCallback()) {
            video->renderFrame();
        }
    }
}

size_t LibretroDroid::handleAudioCallback(const int16_t *data, size_t frames) {
    if (audio && audioEnabled) {
        audio->write(data, frames);
    }
    return frames;
}

int16_t LibretroDroid::handleSetInputState(
    unsigned int port,
    unsigned int device,
    unsigned int index,
    unsigned int id
) {
    if (input) {
        return input->getInputState(port, device, index, id);
    }
    return 0;
}

uintptr_t LibretroDroid::handleGetCurrentFrameBuffer() {
    if (video) {
        return video->getCurrentFramebuffer();
    }
    return 0;
}

void LibretroDroid::reset() {
    std::lock_guard<std::mutex> lock(coreLock);

    core->retro_reset();
}

std::pair<int8_t*, size_t> LibretroDroid::serializeState() {
    std::lock_guard<std::mutex> lock(coreLock);

    size_t size = core->retro_serialize_size();
    if (size == 0) {
        LOGE("retro_serialize_size returned 0");
        return std::pair(nullptr, 0);
    }
    auto data = new int8_t[size];

    bool success = core->retro_serialize(data, size);
    if (!success) {
        LOGE("retro_serialize failed for size %zu", size);
        delete[] data;
        return std::pair(nullptr, 0);
    }

    LOGI("retro_serialize succeeded with size %zu", size);
    return std::pair(data, size);
}

void LibretroDroid::resetCheat() {
    std::lock_guard<std::mutex> lock(coreLock);

    core->retro_cheat_reset();
}

void LibretroDroid::setCheat(unsigned index, bool enabled, const std::string& code) {
    std::lock_guard<std::mutex> lock(coreLock);

    core->retro_cheat_set(index, enabled, Utils::cloneToCString(code));
}

bool LibretroDroid::requiresVideoRefresh() const {
    return dirtyVideo;
}

void LibretroDroid::clearRequiresVideoRefresh() {
    dirtyVideo = false;
}

void LibretroDroid::afterGameLoad() {
    struct retro_system_av_info system_av_info {};
    core->retro_get_system_av_info(&system_av_info);

    fpsSync = std::make_unique<FPSSync>(system_av_info.timing.fps, screenRefreshRate);

    double inputSampleRate = system_av_info.timing.sample_rate * fpsSync->getTimeStretchFactor();

    audio = std::make_unique<Audio>(
        (int32_t) std::lround(inputSampleRate),
        system_av_info.timing.fps,
        preferLowLatencyAudio
    );

    updateAudioSampleRateMultiplier();

    defaultAspectRatio = findDefaultAspectRatio(system_av_info);

    VulkanContext::getInstance().setTargetFrameRate(static_cast<float>(system_av_info.timing.fps));
    if (system_av_info.timing.fps > 0.0) {
        int64_t targetDurationNanos = static_cast<int64_t>(1e9 / system_av_info.timing.fps);
        ADPFManager::getInstance().startSession(gettid(), targetDurationNanos);
    }
}

float LibretroDroid::findDefaultAspectRatio(const retro_system_av_info& system_av_info) {
    float result = system_av_info.geometry.aspect_ratio;
    if (result < 0) {
        result =
            (float) system_av_info.geometry.base_width / (float) system_av_info.geometry.base_height;
    }
    return result;
}

void LibretroDroid::handleRumbleUpdates(const std::function<void(int, float, float)> &handler) {
    if (rumble && rumbleEnabled) {
        rumble->handleRumbleUpdates(handler);
    }
}

void LibretroDroid::setViewport(Rect viewportRect) {
    this->viewportRect = viewportRect;

    if (video != nullptr) {
        video->updateViewportSize(viewportRect);
    }
    if (vulkanRenderer != nullptr) {
        vulkanRenderer->updateViewport(viewportRect);
    }
}

void LibretroDroid::setViewportAlignment(unsigned int viewportAlignment) {
    this->viewportAlignment = viewportAlignment;

    if (video) {
        this->video->updateViewportAlignment(viewportAlignment);
    }
    if (vulkanRenderer) {
        this->vulkanRenderer->updateViewportAlignment(viewportAlignment);
    }
}

} //namespace libretrodroid
