# gpu_share

## Goal

Build a Chrome-tab-style architecture for GPU-accelerated rendering: a **presenter** process owns the window and composites frames from **renderer** processes that run headless. GPU surfaces are shared zero-copy via Vulkan external memory extensions. Renderer processes can be written in C++, Rust, or TypeScript — the presenter doesn't care what language produced the pixels.

The long-term vision is a platform where untrusted/sandboxed renderers (potentially running user-provided TypeScript via embedded Deno/V8) produce GPU frames that the presenter displays, with input events relayed back. Like Chrome tabs, but for GPU-rendered content.

## Architecture

```
Presenter (C++, SDL3 + Vulkan)
    |
    |  TCP 127.0.0.1:9710
    |  - PID exchange after connect
    |  - SharedSurfaceInfo + Vulkan memory handle (fd/HANDLE)
    |  - InputEvent stream (keyboard, mouse, scroll, resize)
    |
    +--- test_renderer (C++, embedded SPIR-V triangle)
    |    or
    +--- deno_renderer (Rust + deno_core, WGSL shaders from TypeScript)
```

### Handle passing over TCP
- **Linux**: sender sends raw fd as uint64, receiver clones via `pidfd_open` + `pidfd_getfd` (kernel 5.6+)
- **Windows**: sender calls `DuplicateHandle` into remote process, sends duplicated handle value as uint64

### Key data structures
- `SharedSurfaceInfo` (shared/shared_handle.h) — 20 bytes, image dimensions + format + memory info
- `InputEvent` (shared/input_event.h) — 20 bytes, tagged union for mouse/keyboard/scroll/resize
- Both must match exactly between C++ and Rust (verified by compile-time size asserts)

## What exists today

### Presenter (`presenter/`)
- C++17, SDL3 + Vulkan
- Creates window, Vulkan swapchain, imports shared VkImage from renderer
- Blits renderer's image to swapchain each frame
- Forwards SDL input events to renderer via TCP
- Spawns renderer as child process, terminates on exit
- Handles window resize (recreates swapchain, tells renderer to resize shared image)
- `--deno` flag selects deno_renderer, default is test_renderer
- `--no-spawn` for manual renderer launch, `--port` to override TCP port

### Test renderer (`test_renderer/`)
- C++17, headless Vulkan
- Spinning RGB triangle with embedded SPIR-V shaders (compiled from GLSL)
- Full graphics pipeline with push constants (angle, aspect ratio, scale)
- Interactive: mouse drag rotates, scroll zooms, space pauses
- Handles resize events from presenter

### Deno renderer (`deno_renderer/`)
- Rust, headless Vulkan via ash, TypeScript via deno_core (V8)
- WGSL shader compilation via naga (same compiler wgpu/WebGPU uses)
- TypeScript (`scene.ts`) defines shaders, creates pipelines, handles input per-frame
- WebGPU-shaped ops: `op_gpu_create_shader_module`, `op_gpu_create_render_pipeline`, `op_gpu_draw`, `op_gpu_poll_events`, `op_gpu_set_rotation`, etc.
- Per-frame JS callback (`globalThis.__frame`) receives elapsed time, polls input events
- Reader thread + mpsc channel for reliable TCP event delivery
- Cross-platform: Linux + Windows (Vulkan external memory fd/win32)

### Transport (`handle_transport/`)
- Unified TCP implementation for both platforms
- Single file: `handle_transport_tcp.cpp`
- PID exchange, handle passing, plain data send/recv, non-blocking recv, cancellable accept

### Godot GDExtension scaffolding (`renderer_extension/`)
- Scaffolded but not yet integrated
- Would allow Godot 4.x as a renderer process

## Build

Requires: Vulkan SDK, CMake 3.25+, C++17 compiler. Rust toolchain for deno_renderer.

```bash
# Everything (C++ + Rust if cargo is on PATH)
cmake -B build
cmake --build build --config Debug

# Run with C++ triangle renderer (default)
build/presenter/Debug/presenter.exe

# Run with Rust/TypeScript renderer
build/presenter/Debug/presenter.exe --deno
```

## What's next

### Near-term
- Integrate wgpu-core so TypeScript gets the real WebGPU API (not just our custom ops)
  - Use `Global::from_hal_instance()` to wrap our ash Instance
  - `CreateDeviceCallback` to inject external memory extensions
  - `create_texture_from_hal()` to wrap the exportable image as a WebGPU texture
  - TypeScript owns the render loop via `requestAnimationFrame`-style callback
- Multiple renderer tabs (presenter composites N surfaces, tab bar UI)
- Godot 4.x renderer integration via the GDExtension

### Medium-term
- gRPC control plane (replace raw TCP for production use)
- Renderer crash recovery (detect dead process, show placeholder, re-launch)
- Resize debouncing (coalesce rapid resize events)
- Hot-reload TypeScript scene scripts without restarting

### Long-term
- Sandboxed renderer execution (WASM or Deno permissions model)
- Multiple windows / multi-monitor
- Shared GPU semaphores for frame-perfect sync (currently unsynchronized mailbox)
- Performance: profile import overhead, frame latency, skip rate

## Conventions
- C++ code uses `VK_CHECK` macro for Vulkan errors
- Rust code uses `ash` for Vulkan, `naga` for WGSL compilation, `deno_core` for V8
- TCP port 9710 is the default for all components
- `--port` / `-p` overrides it everywhere
- Presenter auto-finds renderer executables relative to itself or the working directory
