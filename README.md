# Omni-Libretrodroid

**Omni-Libretrodroid** is a modernized, high-performance Android bridge and renderer for Libretro emulator cores, built as an advanced evolution of the original `libretrodroid` library specifically engineered for [Omnidroid](https://github.com/Ahmed-Abousaif/Omnidroid).

---

## 🚀 Key Improvements & Architectural Additions

### 1. 🌋 Native Vulkan Hardware Acceleration Engine
* **Full Vulkan Core Support**: Implemented a standalone Vulkan rendering backend supporting `RETRO_HW_CONTEXT_VULKAN` via `RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE` and `RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE`.
* **Surface Lifecycle & Synchronization**: Manages native `ANativeWindow` swapchains, multi-frame synchronization (semaphores, fences), image layout transitions, and descriptor sets.
* **Zero-Copy Pipeline**: Delivers optimal frame delivery with minimal GPU-to-CPU roundtrips for Vulkan-native cores (e.g., Citra/Azahar, Dolphin, PPSSPP).

### 2. 🛡️ OpenGL ES State Isolation & VAO Protection
* **Full State Preservation**: Complex 3D cores (such as Dolphin and Citra) manipulate low-level OpenGL ES state directly. Omni-Libretrodroid snapshots and isolates:
  * Framebuffer (FBO), Vertex Array Objects (VAO), Vertex Buffer Objects (VBO), and Element Buffer Objects (EBO).
  * Active shader programs, texture bindings across units, viewports, depth/stencil buffers, scissor tests, cull face, alpha blending, dithering, and color/depth write masks.
* **Frontend Dedicated Buffers**: Encapsulates frontend quad rendering within dedicated VAO/VBO instances (`vboPosition`, `vboCoordinate`), preventing state collision and visual glitches.

### 3. 📐 Dynamic Resolution Scaling & FBO Reallocation
* **Adaptive Framebuffers**: `FramebufferRenderer` dynamically tracks rendered dimensions in `onNewFrame`.
* **Crisp Multi-Resolution Scaling**: When internal resolution scaling is modified at runtime (e.g., 1x Native up to 2x, 3x, 4x, or 6x HD/4K in Dolphin or 3DS), the intermediate FBO and shader passes automatically reallocate to match core geometry, preventing viewport clipping or corner-shrinking artifacts.

### 4. 📱 16KB Page Size Alignment & Android 16 Readiness
* **Modern Memory Architecture**: Compiled with `-Wl,-z,max-page-size=16384` across all native C/C++ targets (`arm64-v8a`, `x86_64`, etc.), ensuring full compliance with Android 15 & 16 16KB memory page devices.
* **Android 14+ `fdsan` Safety**: Sanitized file descriptor handling in `DetachedVirtualFile` and `VFS` to prevent Android runtime aborts caused by descriptor ownership sanitizers.

### 5. ⚙️ Libretro V2 Core Options & Extended Environment Support
* **Libretro V2 Core Options**: Full support for `RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2` and `RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL` in addition to legacy V1 definitions.
* **Intl & Multi-Option Localization**: Formats option strings for UI rendering and localized option display.
* **Microphone Interface**: Implemented `RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE` for cores requiring audio input (e.g., melonDS).
* **Virtual File System (VFS)**: Complete `RETRO_ENVIRONMENT_GET_VFS_INTERFACE` supporting scoped storage and direct file streaming.
* **Dynamic Aspect Ratio & Geometry**: Real-time geometry updates (`RETRO_ENVIRONMENT_SET_GEOMETRY`) and rotation changes (`RETRO_ENVIRONMENT_SET_ROTATION`).

### 6. 💾 Robust Save State & SRAM Architecture
* **Safety & Bounds Checking**: Native validation for `retro_serialize_size()`, `retro_serialize()`, and `retro_unserialize()`.
* **Zero Memory Leaks**: Fixed native buffer leaks during JNI state and SRAM transfers by ensuring proper deallocation of C++ heap buffers.
* **Nullable SRAM Handling**: Transparently handles modern cores (Dolphin, Citra) that save directly to memory cards or NAND rather than Libretro's SRAM buffer.
* **Thread-Safe Emulation Dispatch**: `runOnEmulationThread` in `GLRetroView` utilizes robust `try/finally` latch releases to avoid thread deadlocks or crashes on null serialization outputs.

### 7. 🎮 Multi-Controller & Port Routing
* **Runtime Port Configuration**: Dynamically configures controller device types (`setControllerType`) per port (e.g., Sideways Wiimote, Wiimote + Nunchuk, Classic Controller).
* **Port Disconnection**: Allows explicit disconnection of unused player ports (`RETRO_DEVICE_NONE`) to prevent ghost input detection.
* **Safe Initialization Sync**: Guarantees core initialization before input dispatch via `waitGLEvent<FrameRendered>` to prevent startup race conditions.

---

## 🛠️ Architecture Overview

```
omni-libretrodroid/
├── src/main/cpp/
│   ├── renderers/
│   │   ├── es2/             # OpenGL ES 2.0 Fallback Renderer
│   │   ├── es3/             # OpenGL ES 3.0 Framebuffer & Filter Pipeline
│   │   └── vulkan/          # Vulkan 1.0/1.1 Native Hardware Renderer
│   ├── environment.cpp      # Libretro Environment Callback Dispatcher
│   ├── libretrodroid.cpp    # Core Lifecycle & Emulation Loop
│   ├── libretrodroidjni.cpp # JNI Interface Bridge
│   ├── video.cpp            # Display Layout, Quad Shaders & GL State Guard
│   ├── input.cpp            # Libretro Controller & Touch Routing
│   └── vfs.cpp              # Android Virtual File System Interface
└── src/main/java/
    └── com/omnidroid/libretrodroid/
        ├── GLRetroView.kt   # SurfaceView Component & Thread Dispatcher
        └── LibretroDroid.kt # JNI Native Declarations
```

---

## 📄 License
Omni-Libretrodroid is proprietary and closed source. All rights reserved.