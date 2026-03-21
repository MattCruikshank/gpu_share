# gpu_share

## Goal

Build a Chrome-tab-style architecture for GPU-accelerated rendering: a **presenter** process owns the window and composites frames from **renderer** processes that run headless. GPU surfaces are shared zero-copy via Vulkan external memory extensions. Renderer processes can be written in C++, Rust, or TypeScript — the presenter doesn't care what language produced the pixels.

The long-term vision is a platform where untrusted/sandboxed renderers (potentially running user-provided TypeScript via embedded Deno/V8) produce GPU frames that the presenter displays, with input events relayed back. Like Chrome tabs, but for GPU-rendered content.

## Architecture

```
Presenter (C++, SDL3 + Vulkan, gRPC server)
    |
    |  gRPC 127.0.0.1:9710 (RendererBridge service)
    |  - RegisterSurface: renderer sends PID + SharedSurfaceInfo + memory handle
    |  - StreamInput: server-streams InputEvent (keyboard, mouse, scroll, resize)
    |  - NotifySurface: renderer sends updated surface after resize
    |
    +--- test_renderer (C++, gRPC client, embedded SPIR-V triangle)
    |    or
    +--- deno_renderer (Rust + deno_core, gRPC client via tonic, WGSL from TypeScript)
```

### gRPC transport (`proto/gpu_share.proto`)
- Proto3 schema defines all messages and the `RendererBridge` service
- C++ code generated via protoc + grpc_cpp_plugin (system packages)
- Rust code generated via tonic-prost-build in `deno_renderer/build.rs`
- Protobuf `InputEvent` uses `oneof` for typed event variants — no more raw byte casting

### Handle passing over gRPC
- Memory handles are passed as `uint64` fields in protobuf messages
- **Linux**: receiver clones via `pidfd_open` + `pidfd_getfd` (kernel 5.6+)
- **Windows**: sender calls `DuplicateHandle` into remote process, sends duplicated handle value

### Key data structures
- `SharedSurfaceInfo` — defined in both `proto/gpu_share.proto` (wire format) and `shared/shared_handle.h` (internal Vulkan use)
- `InputEvent` — defined in `proto/gpu_share.proto` as a `oneof` with typed message variants
- Old C ABI structs in `shared/input_event.h` are no longer used on the wire

## What exists today

### Presenter (`presenter/`)
- C++17, SDL3 + Vulkan, gRPC server (`grpc_server.h/cpp`)
- Creates window, Vulkan swapchain, imports shared VkImage from renderer
- Blits renderer's image to swapchain each frame
- gRPC `RendererBridge` service streams input events to renderer
- Spawns renderer as child process, terminates on exit
- Handles window resize (recreates swapchain, tells renderer to resize shared image)
- `--deno` flag selects deno_renderer, default is test_renderer
- `--no-spawn` for manual renderer launch, `--port` to override gRPC port

### Test renderer (`test_renderer/`)
- C++17, headless Vulkan, gRPC client
- Spinning RGB triangle with embedded SPIR-V shaders (compiled from GLSL)
- Full graphics pipeline with push constants (angle, aspect ratio, scale)
- Interactive: mouse drag rotates, scroll zooms, space pauses
- Receives typed protobuf InputEvents via StreamInput server-stream
- Handles resize via NotifySurface RPC

### Deno renderer (`deno_renderer/`)
- Rust, headless Vulkan via ash, TypeScript via deno_core (V8), gRPC client via tonic
- WGSL shader compilation via naga (same compiler wgpu/WebGPU uses)
- TypeScript (`scene.ts`) defines shaders, creates pipelines, handles input per-frame
- WebGPU-shaped ops: `op_gpu_create_shader_module`, `op_gpu_create_render_pipeline`, `op_gpu_draw`, `op_gpu_poll_events`, `op_gpu_set_rotation`, etc.
- Per-frame JS callback (`globalThis.__frame`) receives elapsed time, polls input events
- Typed protobuf events received via tokio mpsc channel from StreamInput background task
- Cross-platform: Linux + Windows (Vulkan external memory fd/win32)

### Transport (legacy: `handle_transport/`)
- Old TCP implementation, no longer compiled into any target
- Kept for reference; superseded by gRPC

### Godot GDExtension scaffolding (`renderer_extension/`)
- Scaffolded but not yet integrated
- Would allow Godot 4.x as a renderer process

## Build

Requires: Vulkan SDK, CMake 3.25+, C++17 compiler, gRPC + protobuf. Rust toolchain + protoc for deno_renderer. Node.js + npm for TypeScript protobuf bundle.

### Linux
```bash
sudo apt install libgrpc++-dev protobuf-compiler-grpc
cmake -B build
cmake --build build
```

### Windows (vcpkg recommended)
```powershell
# Install gRPC via vcpkg (one-time)
vcpkg install grpc:x64-windows protobuf:x64-windows

# Build with vcpkg toolchain
cmake -B build -DCMAKE_TOOLCHAIN_FILE="[vcpkg root]/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug
```

If vcpkg is not available, CMake falls back to FetchContent (builds gRPC from source, slow first build).

### Run
```bash
# C++ triangle renderer (default)
build/presenter/Debug/presenter.exe

# Rust/TypeScript renderer
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
- gRPC port 9710 is the default for all components
- `--port` / `-p` overrides it everywhere
- Proto schema is the single source of truth for wire types (`proto/gpu_share.proto`)
- Presenter auto-finds renderer executables relative to itself or the working directory
