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

#ifndef LIBRETRODROID_LOG_H
#define LIBRETRODROID_LOG_H

#define MODULE_NAME "libretrodroid"

#ifdef NDEBUG
#define VERBOSE_LOGGING 0
#define GLES_LOGGING 0
#else
#define VERBOSE_LOGGING 1
#define GLES_LOGGING 1
#endif

#if GLES_LOGGING
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#endif

#include <android/log.h>

#if VERBOSE_LOGGING

#define LOGV(...) __android_log_print(ANDROID_LOG_INFO, MODULE_NAME, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_INFO, MODULE_NAME, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MODULE_NAME, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, MODULE_NAME, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, MODULE_NAME, __VA_ARGS__)
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL, MODULE_NAME, __VA_ARGS__)

#else

#define LOGV(...) ((void)0)
#define LOGD(...) ((void)0)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, MODULE_NAME, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, MODULE_NAME, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, MODULE_NAME, __VA_ARGS__)
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL, MODULE_NAME, __VA_ARGS__)

#endif

#if GLES_LOGGING

[[maybe_unused]] static void MessageCallback(
    GLenum source,
    GLenum type,
    GLuint id,
    GLenum severity,
    GLsizei length,
    const GLchar* message,
    const void* userParam
) {
    if (type == GL_DEBUG_TYPE_ERROR) {
        LOGE("GL CALLBACK: \"** GL ERROR **\" type = 0x%x, severity = 0x%x, message = %s\n",
             type,
             severity,
             message);
    }
}

[[maybe_unused]] inline bool initializeGLESLogCallbackIfNeeded() {
    auto debugCallback = (void (*)(void *, void *)) eglGetProcAddress("glDebugMessageCallback");
    if (debugCallback) {
        glEnable(GL_DEBUG_OUTPUT);
        debugCallback((void*) MessageCallback, nullptr);
    }
    return true;
}

#else

[[maybe_unused]] inline bool initializeGLESLogCallbackIfNeeded() {
    return false;
}

#endif

#endif //LIBRETRODROID_LOG_H
