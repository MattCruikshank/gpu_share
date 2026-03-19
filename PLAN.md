# GPU-Shared Multi-Process Game Window

## Overview

A Chrome-tab-style architecture where a **presenter process** (SDL3 + Vulkan) displays frames produced by multiple **Godot 4.x renderer processes** running offscreen. GPU surfaces are shared zero-copy via Kronos (Vulkan) external memory extensions; control and input flow over gRPC. All glue code in C++. Cross-platform: Linux + Windows.

```
┌─────────────────────────────────────────────────────┐
│                  Presenter Process                  │
│  SDL3 window ─► Vulkan swapchain                    │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐         │
│  │ Tab 0 tex │ │ Tab 1 tex │ │ Tab N tex │  ...    │
│  │ (imported) │ │ (imported) │ │ (imported) │         │
│  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘         │
│        │              │              │               │
│  composite active tab onto swapchain image           │
│  relay input events ──► gRPC ──► renderer            │
└────────┬──────────────┬──────────────┬──────────────┘
         │ fd / handle  │              │
   ┌─────▼─────┐  ┌─────▼─────┐  ┌────▼──────┐
   │ Godot 0   │  │ Godot 1   │  │ Godot N   │
   │ (headless)│  │ (headless)│  │ (headless)│
   │ VkImage   │  │ VkImage   │  │ VkImage   │
   │ exported  │  │ exported  │  │ exported  │
   └───────────┘  └───────────┘  └───────────┘
```

---

## 1. Vulkan External Memory Sharing

### 1.1 Extensions required

| Extension | Linux | Windows |
|-----------|-------|---------|
| `VK_KHR_external_memory` | yes | yes |
| `VK_KHR_external_memory_fd` | yes | — |
| `VK_KHR_external_memory_win32` | — | yes |
| `VK_KHR_external_semaphore` | yes | yes |
| `VK_KHR_external_semaphore_fd` | yes | — |
| `VK_KHR_external_semaphore_win32` | — | yes |

### 1.2 Renderer side (Godot GDExtension)

1. Allocate a `VkImage` with `VkExternalMemoryImageCreateInfo` specifying `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` (Linux) or `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT` (Windows).
2. Back it with `VkDeviceMemory` allocated via `VkExportMemoryAllocateInfo`.
3. Create a pair of `VkSemaphore` with `VkExportSemaphoreCreateInfo` — one for "render complete", one for "presenter done reading".
4. Obtain the fd / HANDLE via `vkGetMemoryFdKHR` / `vkGetMemoryWin32HandleKHR`.
5. After each frame, signal the "render complete" semaphore, then notify presenter via gRPC that a new frame is ready.

### 1.3 Presenter side

1. Receive fd / HANDLE + image metadata (format, extent, memory size, memory type bits) via gRPC.
2. Import `VkDeviceMemory` via `VkImportMemoryFdInfoKHR` / `VkImportMemoryWin32HandleInfoKHR`.
3. Create a `VkImage` bound to that memory with matching parameters.
4. Import the two semaphores.
5. In the render loop: wait on "render complete" semaphore, sample the imported image, signal "presenter done" semaphore.
6. Composite onto swapchain image (fullscreen blit or quad draw for the active tab).

### 1.4 Mailbox model (latest-wins)

Each renderer maintains a ring of **2 shared images** (double-buffer):
- Renderer writes to buffer A, then flips to B, signals gRPC "frame ready" with buffer index.
- Presenter always grabs the most recent buffer index, skipping stale ones.
- Semaphore pair per buffer ensures no read/write collision.

---

## 2. Godot 4.x Integration

### 2.1 GDExtension plugin: `SharedSurfaceRenderer`

- Hooks into Godot's `RenderingServer` to obtain the final rendered frame.
- **Approach**: Use `RenderingServer::get_rendering_device()` to access the `RenderingDevice` (Godot's Vulkan abstraction), then use its low-level API or a custom `RenderingDevice` extension to allocate the exportable image and blit the final viewport into it.
- Alternative: Fork/patch Godot's `VulkanContext` to replace or wrap the swapchain image with an exportable image when running headless.

### 2.2 Headless mode

Launch Godot with `--headless` or `--rendering-driver vulkan` with no window. The GDExtension captures the viewport output each frame.

### 2.3 Scene lifecycle

- Presenter tells renderer (via gRPC) which scene/project to load.
- Renderer responds with shared surface metadata (fd, format, extent).
- Presenter imports and starts displaying.
- On tab switch, presenter stops reading from old renderer's surface, starts reading from new one.

---

## 3. gRPC Interface

### 3.1 Proto definitions

```protobuf
syntax = "proto3";
package gamewindow;

service RendererControl {
  // Presenter → Renderer
  rpc LoadScene(LoadSceneRequest) returns (LoadSceneResponse);
  rpc Resize(ResizeRequest) returns (ResizeResponse);
  rpc SendInput(stream InputEvent) returns (google.protobuf.Empty);
  rpc Shutdown(google.protobuf.Empty) returns (google.protobuf.Empty);
}

service PresenterNotify {
  // Renderer → Presenter
  rpc FrameReady(stream FrameReadyEvent) returns (google.protobuf.Empty);
  rpc RendererStatus(stream StatusEvent) returns (google.protobuf.Empty);
}

message SharedSurfaceInfo {
  uint32 width = 1;
  uint32 height = 2;
  uint32 format = 3;         // VkFormat
  uint64 memory_size = 4;
  uint32 memory_type_bits = 5;
  uint32 buffer_count = 6;   // number of mailbox buffers (2)
  // fd / handle passed out-of-band via SCM_RIGHTS (Linux) or
  // DuplicateHandle (Windows) — NOT in the protobuf payload
}

message LoadSceneRequest {
  string project_path = 1;
  string scene_path = 2;
  uint32 width = 3;
  uint32 height = 4;
}

message LoadSceneResponse {
  SharedSurfaceInfo surface = 1;
  string renderer_id = 2;
}

message FrameReadyEvent {
  string renderer_id = 1;
  uint32 buffer_index = 2;
  uint64 frame_number = 3;
}

message InputEvent {
  string renderer_id = 1;
  oneof event {
    KeyEvent key = 2;
    MouseMotionEvent mouse_motion = 3;
    MouseButtonEvent mouse_button = 4;
    GamepadEvent gamepad = 5;
  }
}

message KeyEvent {
  uint32 scancode = 1;
  bool pressed = 2;
  uint32 modifiers = 3;
}

message MouseMotionEvent {
  float x = 1;
  float y = 2;
  float rel_x = 3;
  float rel_y = 4;
}

message MouseButtonEvent {
  uint32 button = 1;
  bool pressed = 2;
  float x = 3;
  float y = 4;
}

message GamepadEvent {
  uint32 device = 1;
  uint32 axis_or_button = 2;
  float value = 3;
  bool is_axis = 4;
}
```

### 3.2 Handle passing (out-of-band)

gRPC cannot pass file descriptors directly. Two options:

- **Linux**: Establish a side-channel Unix domain socket. Use `sendmsg` with `SCM_RIGHTS` to pass DMA-BUF fds alongside the gRPC `LoadSceneResponse`.
- **Windows**: Renderer calls `DuplicateHandle` targeting the presenter's process handle (obtained via gRPC PID exchange) to inject the `HANDLE` into the presenter process.

---

## 4. Presenter Architecture (SDL3 + Vulkan + C++)

### 4.1 Components

```
presenter/
├── main.cpp                    # Entry point, SDL3 init
├── vulkan_context.h/cpp        # VkInstance, VkDevice, swapchain
├── surface_import.h/cpp        # Import external memory/semaphores
├── compositor.h/cpp            # Blit/draw active tab to swapchain
├── tab_manager.h/cpp           # Track renderer processes, active tab
├── input_relay.h/cpp           # SDL events → gRPC InputEvent stream
├── grpc_client.h/cpp           # gRPC stubs for RendererControl
├── grpc_server.h/cpp           # gRPC service for PresenterNotify
├── handle_passing_linux.h/cpp  # SCM_RIGHTS fd transfer
├── handle_passing_win32.h/cpp  # DuplicateHandle
└── CMakeLists.txt
```

### 4.2 Main loop

```
while running:
    SDL_PollEvent → input_relay.forward(active_tab, event)
    check gRPC FrameReady queue → update active tab's latest buffer_index
    vkAcquireNextImageKHR (swapchain)
    vkWaitSemaphore (active tab's render_complete[buffer_index])
    record cmd buffer:
        transition imported image → SHADER_READ_ONLY
        draw fullscreen quad sampling imported image
        transition swapchain image → PRESENT_SRC
    vkQueueSubmit (signal: presenter_done[buffer_index], swapchain_ready)
    vkQueuePresentKHR
```

### 4.3 Tab switching

- Maintain a `std::unordered_map<std::string, TabState>` keyed by renderer_id.
- `TabState` holds: imported VkImage per buffer, imported semaphores, latest buffer index, gRPC stream handles.
- Tab switch = change which `TabState` the compositor reads from. No GPU resource teardown needed.

---

## 5. Renderer GDExtension Architecture

```
renderer_extension/
├── shared_surface.h/cpp        # Exportable VkImage + semaphore creation
├── frame_capture.h/cpp         # Hook RenderingDevice, blit viewport → shared surface
├── grpc_server.h/cpp           # RendererControl service implementation
├── grpc_client.h/cpp           # PresenterNotify client (FrameReady stream)
├── input_injector.h/cpp        # Convert gRPC InputEvent → Godot InputEvent
├── handle_passing_linux.h/cpp
├── handle_passing_win32.h/cpp
├── register_types.h/cpp        # GDExtension entry point
├── shared_surface.gdextension
└── SConstruct / CMakeLists.txt
```

---

## 6. Build System

- **CMake** for the presenter.
- **SCons** or **CMake** for the GDExtension (SCons is Godot-native).
- Dependencies:
  - Vulkan SDK (lunarg)
  - SDL3 (via FetchContent or system)
  - gRPC + protobuf (via FetchContent or vcpkg)
  - godot-cpp (GDExtension bindings, git submodule)

---

## 7. Platform Abstraction

```cpp
// shared_handle.h
#ifdef _WIN32
  using SharedMemoryHandle = HANDLE;
  using SharedSemaphoreHandle = HANDLE;
  constexpr auto kExternalMemoryHandleType =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  constexpr auto kExternalSemaphoreHandleType =
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
  using SharedMemoryHandle = int; // fd
  using SharedSemaphoreHandle = int;
  constexpr auto kExternalMemoryHandleType =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
  constexpr auto kExternalSemaphoreHandleType =
      VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
#endif
```

---

## 8. Implementation Phases

### Phase 1: Single-renderer proof of concept (Linux)
1. Build presenter with SDL3 + Vulkan swapchain (no shared memory yet — just a colored quad).
2. Build GDExtension that allocates an exportable VkImage and blits Godot's viewport into it.
3. Hard-code fd passing over a Unix socket.
4. Import in presenter, display Godot's output in the window.
5. Validate with `VK_LAYER_KHRONOS_validation`.

### Phase 2: gRPC control plane
1. Define .proto, generate C++ stubs.
2. Implement `LoadScene` / `FrameReady` / `SendInput` flows.
3. Replace hard-coded fd passing with gRPC + SCM_RIGHTS side-channel.

### Phase 3: Input relay
1. Capture SDL3 events in presenter.
2. Stream as `InputEvent` to renderer via gRPC.
3. GDExtension injects into Godot's `Input` singleton.

### Phase 4: Multi-tab
1. Tab manager in presenter: spawn/kill Godot renderer processes.
2. Each renderer registers via gRPC, exports its surface.
3. Presenter imports N surfaces, composites the active one.
4. Tab switching, tab bar UI (could be Dear ImGui overlay).

### Phase 5: Windows support
1. Swap fd-based paths for `HANDLE`-based (`VK_KHR_external_memory_win32`).
2. Replace `SCM_RIGHTS` with `DuplicateHandle` for handle passing.
3. Test on Windows with Vulkan-capable GPU.

### Phase 6: Polish
1. Resize handling (re-export/re-import surfaces on resize).
2. Renderer crash recovery (detect dead process, show placeholder, allow re-launch).
3. Performance profiling: measure import overhead, frame latency, skip rate.
4. Optional: compositor effects (tab transitions, picture-in-picture).

---

## 9. Key Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Godot's `RenderingDevice` doesn't expose raw VkImage handles | Blocks export | Use `RenderingDevice::texture_get_native_handle()` (added in 4.3) or patch Godot source |
| GPU driver doesn't support external memory between processes | No sharing | Validate at startup with `vkGetPhysicalDeviceExternalImageFormatProperties`; fallback to CPU readback (slow) |
| Semaphore sync bugs cause visual corruption | Glitches | Strict validation layers; per-buffer fencing; mailbox model avoids most races |
| gRPC latency too high for frame notifications | Stale frames | Use gRPC bidirectional streaming (persistent connection); frame notification is <1KB |
| Handle passing differs wildly Linux vs Windows | Platform pain | Abstract behind `SharedHandle` interface from Phase 1 |

---

## 10. Dependencies & Versions

| Dependency | Version | Notes |
|------------|---------|-------|
| Vulkan SDK | 1.3+ | External memory/semaphore extensions stable since 1.1 |
| SDL3 | 3.x latest | Vulkan surface creation, input events |
| Godot | 4.3+ | `texture_get_native_handle()`, headless mode improvements |
| godot-cpp | matching Godot 4.3+ | GDExtension C++ bindings |
| gRPC | 1.60+ | C++ codegen, bidirectional streaming |
| protobuf | 25+ | Matching gRPC requirements |
| CMake | 3.25+ | FetchContent, presets |
