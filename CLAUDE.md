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

### Deno renderer (`deno_renderer/`)
- Rust, headless Vulkan via ash, TypeScript via deno_core (V8), gRPC client via tonic
- **Standard WebGPU API** via forked deno_webgpu (from Deno v2.7.7, wgpu-core 28)
- `wgpu_hal_bridge.rs` wraps ash Vulkan objects as wgpu-core Global/Adapter/Device
- Shared VkImage imported as wgpu-core Texture via `create_texture_from_hal`
- `prepare_for_present()` uses wgpu-core command encoder to transition shared texture to COPY_SRC (= TRANSFER_SRC_OPTIMAL) after each frame, keeping wgpu-core's layout tracker in sync
- Scene scripts use standard WebGPU API: `createShaderModule`, `createRenderPipeline`, `beginRenderPass`, `queue.submit`
- Legacy custom ops (`op_gpu_create_shader_module`, etc.) still present for backward compatibility
- Per-frame JS callback (`globalThis.__frame`) receives elapsed time
- `op_gpu_poll_events` returns length-prefixed protobuf bytes → decoded in TS via `proto.decodeEvents()`
- `webgpu_shim.js` bootstraps `navigator.gpu` + `GPUBufferUsage` in bare V8
- `v8_polyfill.js` provides TextEncoder/TextDecoder for bare V8 environment
- `proto_bundle.js` is an esbuild IIFE bundle of protobuf-es runtime + generated types
- Supports TabPause/TabResume: sleeps at ~2fps when paused, ~60fps when active
- Cross-platform: Linux + Windows (Vulkan external memory fd/win32)

### Scenes (`deno_renderer/scenes/`)
- `1.ts` — Spinning RGB triangle (mouse drag rotates, scroll zooms, space pauses)
- `2.ts` — Bouncing octagon (space pauses bounce)

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
# Presenter opens, press 1-9 to launch tabs, R to reload, ESC to quit
build/presenter/Debug/presenter.exe
```

## What's next

### Near-term
- **Tab bar UI overlay** — visual indicator of active tab, clickable tab switching

### Medium-term
- **Sandboxed renderer execution** — run untrusted scene scripts in a sandboxed environment (Deno permissions model or WASM). Restrict file system access, network, and system calls while allowing GPU rendering.
- **Shared GPU semaphores for frame-perfect sync** — currently the presenter reads the shared image unsynchronized (mailbox model). Add `VK_KHR_external_semaphore` to coordinate renderer writes and presenter reads, eliminating potential tearing.
- **Performance profiling** — measure and optimize import overhead, frame latency, frame skip rate. Add optional `--perf` flag to log timing data.

### Long-term
- **Multiple windows / multi-monitor** — allow renderers to target different windows or monitors
- **Keep background tabs rendering** — option to render background tabs at full rate instead of 2fps throttle (for use cases like live previews)
- **Hot-reload scenes** — watch scene script files for changes and reload without restarting the renderer process

## Conventions
- C++ code uses `VK_CHECK` macro for Vulkan errors
- Rust code uses `ash` for Vulkan, `wgpu-core` + forked `deno_webgpu` for WebGPU API, `deno_core` for V8
- gRPC base port 9710, each tab uses `9710 + tab_index`
- `--port` / `-p` overrides the base port
- Proto schema is the single source of truth for wire types (`proto/gpu_share.proto`)
- Presenter auto-finds deno_renderer executable relative to itself
- Scene scripts resolved relative to the renderer executable (`scenes/<N>.ts`)
