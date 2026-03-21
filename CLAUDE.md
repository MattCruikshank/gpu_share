# gpu_share

## Goal

Build a Chrome-tab-style architecture for GPU-accelerated rendering: a **presenter** process owns the window and composites frames from **renderer** processes that run headless. GPU surfaces are shared zero-copy via Vulkan external memory extensions. Renderer processes can be written in C++, Rust, or TypeScript — the presenter doesn't care what language produced the pixels.

The long-term vision is a platform where untrusted/sandboxed renderers (potentially running user-provided TypeScript via embedded Deno/V8) produce GPU frames that the presenter displays, with input events relayed back. Like Chrome tabs, but for GPU-rendered content.

## Architecture

```
Presenter (C++, SDL3 + Vulkan, multi-tab)
    |
    |  Per-tab gRPC server (ports 9710, 9711, 9712, ...)
    |  RendererBridge service:
    |  - RegisterSurface: renderer sends PID + SharedSurfaceInfo + memory handle
    |  - StreamInput: server-streams InputEvent (keyboard, mouse, scroll, resize, pause/resume)
    |  - NotifySurface: renderer sends updated surface after resize
    |
    +--- Tab 1: deno_renderer → scenes/1.ts (spinning triangle)
    +--- Tab 2: deno_renderer → scenes/2.ts (bouncing octagon)
    +--- Tab N: deno_renderer → scenes/N.ts (user-defined)
```

### Multi-tab model
- Presenter starts with no renderers (black window)
- Number keys 1-9 lazy-spawn a deno_renderer with `scenes/<N>.ts`
- Each tab gets its own gRPC server on port `BASE_PORT + tab_index`
- Switching tabs sends `TabPause` to the old tab, `TabResume` to the new one
- Background tabs sleep at ~2fps to save GPU; active tab runs at ~60fps
- Input events are forwarded only to the active tab
- Window title shows current tab number

### gRPC transport (`proto/gpu_share.proto`)
- Proto3 schema defines all messages and the `RendererBridge` service
- C++ code generated via protoc + grpc_cpp_plugin (system packages or vcpkg)
- Rust code generated via tonic-prost-build in `deno_renderer/build.rs`
- TypeScript types generated via protobuf-es, bundled into `proto_bundle.js`
- Protobuf `InputEvent` uses `oneof` for typed event variants (mouse, keyboard, resize, pause/resume)
- Events flow as protobuf bytes all the way from C++ presenter → Rust → TypeScript scene scripts

### Handle passing over gRPC
- Memory handles are passed as `uint64` fields in protobuf messages
- **Linux**: receiver clones via `pidfd_open` + `pidfd_getfd` (kernel 5.6+)
- **Windows**: sender calls `DuplicateHandle` into remote process, sends duplicated handle value

## What exists today

### Presenter (`presenter/`)
- C++17, SDL3 + Vulkan, multi-tab gRPC server (`grpc_server.h/cpp`)
- Creates window, Vulkan swapchain, imports shared VkImage from active renderer
- Blits active tab's image to swapchain each frame; clears to black if no tab
- Per-tab `GrpcBridge` instance streams input events to the active renderer
- Lazy-spawns deno_renderer child processes on number key press
- Handles window resize (recreates swapchain, tells active renderer to resize)
- `--port` overrides the base gRPC port (default 9710)

### Test renderer (`test_renderer/`)
- C++17, headless Vulkan, gRPC client
- Spinning RGB triangle with embedded SPIR-V shaders (compiled from GLSL)
- Full graphics pipeline with push constants (angle, aspect ratio, scale)
- Interactive: mouse drag rotates, scroll zooms, space pauses
- Receives typed protobuf InputEvents via StreamInput server-stream
- Handles resize via NotifySurface RPC
- Not currently integrated into the multi-tab presenter (standalone use only)

### Deno renderer (`deno_renderer/`)
- Rust, headless Vulkan via ash, TypeScript via deno_core (V8), gRPC client via tonic
- WGSL shader compilation via naga (same compiler wgpu/WebGPU uses)
- Scene scripts in `deno_renderer/scenes/` define shaders, pipelines, and per-frame input handling
- WebGPU-shaped ops: `op_gpu_create_shader_module`, `op_gpu_create_render_pipeline`, `op_gpu_draw`, `op_gpu_poll_events`, `op_gpu_set_rotation`, etc.
- Per-frame JS callback (`globalThis.__frame`) receives elapsed time
- `op_gpu_poll_events` returns length-prefixed protobuf bytes → decoded in TS via `proto.decodeEvents()`
- `v8_polyfill.js` provides TextEncoder/TextDecoder for bare V8 environment
- `proto_bundle.js` is an esbuild IIFE bundle of protobuf-es runtime + generated types
- Supports TabPause/TabResume: sleeps at ~2fps when paused, ~60fps when active
- Cross-platform: Linux + Windows (Vulkan external memory fd/win32)

### Scenes (`deno_renderer/scenes/`)
- `1.ts` — Spinning RGB triangle (mouse drag rotates, scroll zooms, space pauses)
- `2.ts` — Bouncing octagon (space pauses bounce)

### Transport (legacy: `handle_transport/`)
- Old TCP implementation, no longer compiled into any target
- Kept for reference; superseded by gRPC

### Godot GDExtension scaffolding (`renderer_extension/`)
- Scaffolded but not yet integrated
- Would allow Godot 4.x as a renderer process

## Build

Requires: Vulkan SDK, CMake 3.25+, C++17 compiler, gRPC + protobuf. Rust toolchain + protoc for deno_renderer. Node.js + npm for TypeScript protobuf bundle (one-time: `cd deno_renderer && npm install && npm run proto`).

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
# Presenter opens, press 1-9 to launch tabs
build/presenter/Debug/presenter.exe

# Standalone test_renderer (not multi-tab, needs manual --no-spawn presenter)
build/test_renderer/Debug/test_renderer.exe
```

## What's next

### Near-term
- Send resize to all connected tabs (not just active) on window resize
- Integrate wgpu-core so TypeScript gets the real WebGPU API (not just our custom ops)
  - Use `Global::from_hal_instance()` to wrap our ash Instance
  - `CreateDeviceCallback` to inject external memory extensions
  - `create_texture_from_hal()` to wrap the exportable image as a WebGPU texture
  - TypeScript owns the render loop via `requestAnimationFrame`-style callback
- Godot 4.x renderer integration via the GDExtension
- Integrate test_renderer as a tab option (tab 0 or a flag)

### Medium-term
- Renderer crash recovery (detect dead process, show placeholder, re-launch)
- Resize debouncing (coalesce rapid resize events)
- Hot-reload TypeScript scene scripts without restarting
- Tab bar UI overlay

### Long-term
- Sandboxed renderer execution (WASM or Deno permissions model)
- Multiple windows / multi-monitor
- Shared GPU semaphores for frame-perfect sync (currently unsynchronized mailbox)
- Performance: profile import overhead, frame latency, skip rate
- Option to keep background tabs rendering at full rate

## Conventions
- C++ code uses `VK_CHECK` macro for Vulkan errors
- Rust code uses `ash` for Vulkan, `naga` for WGSL compilation, `deno_core` for V8
- gRPC base port 9710, each tab uses `9710 + tab_index`
- `--port` / `-p` overrides the base port
- Proto schema is the single source of truth for wire types (`proto/gpu_share.proto`)
- Presenter auto-finds deno_renderer executable relative to itself
- Scene scripts resolved relative to the renderer executable (`scenes/<N>.ts`)
