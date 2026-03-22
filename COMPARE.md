# gpu_share vs Chrome vs Deno

## Architecture comparison

|  | Chrome | Deno | gpu_share |
|---|---|---|---|
| **Process model** | Browser process + per-tab renderer processes | Single process (or `--unstable-worker` for threads) | Presenter process + per-tab renderer processes |
| **Sandbox** | Renderer processes are sandboxed, can't touch the filesystem or network directly | Permission-gated (`--allow-read`, `--allow-net`, etc.) | Renderers are headless — no window, no input devices, only a shared GPU surface |
| **GPU access** | Renderer composites via Viz process; cross-process GPU sharing via mailboxes + sync tokens | WebGPU via wgpu (in-process) | Each renderer has its own Vulkan device; frames shared zero-copy via `VK_EXTERNAL_MEMORY` |
| **Display surface** | Viz process owns the compositor, blends layers from all renderers | Renderer owns its own window (if any) | Presenter owns the window and swapchain, blits the active tab's shared image |
| **IPC** | Mojo (custom IPC), shared memory, platform channels | N/A (single process) or `postMessage` for workers | gRPC (protobuf over localhost TCP) |
| **Input routing** | Browser process dispatches input to the focused renderer | OS events go directly to the process | Presenter captures SDL events, streams them via gRPC to the active tab |
| **Frame sync** | Sync tokens + mailbox textures between renderer and Viz | N/A (same process) | Exported Vulkan timeline semaphore — renderer signals after each frame, presenter polls before blit |
| **Tab lifecycle** | Browser spawns/kills renderer processes per tab | N/A | Presenter lazy-spawns renderers on keypress, pauses background tabs to ~2fps |
| **Content language** | HTML/CSS/JS (Blink engine) | JS/TS (V8 + Rust ops) | WGSL shaders + JS/TS scene scripts (V8 via deno_core, WebGPU via wgpu) |
| **Render target** | DOM + Skia/ANGLE rasterization into compositor layers | Canvas/WebGPU into a window surface | WebGPU into a shared VkImage (no window, no DOM) |

## What gpu_share borrows from Chrome

- **Process-per-tab isolation.** A misbehaving renderer can't take down the presenter or other tabs. Chrome does this for security and stability; gpu_share does it for the same reasons, plus to allow mixed-language renderers (Rust, TS, eventually sandboxed user code).

- **Background tab throttling.** Chrome throttles background tab timers to 1/sec. gpu_share throttles background renderers to ~2fps via `TabPause`/`TabResume` events.

- **Input routing through the host.** Chrome's browser process decides which renderer gets input based on hit-testing. gpu_share's presenter routes input based on the active tab index — simpler, since there's only one fullscreen surface per tab.

## What gpu_share borrows from Deno

- **V8 + Rust ops model.** Deno embeds V8 and exposes system capabilities via Rust "ops" registered with `deno_core`. gpu_share does the same — `op_gpu_poll_events` returns protobuf bytes, `op_gpu_set_webgpu_active` toggles WebGPU mode, and the WebGPU API itself comes from a fork of `deno_webgpu`.

- **TypeScript scene scripts.** Like Deno running `.ts` files, gpu_share's renderers execute TypeScript scenes. The difference is that gpu_share strips Deno down to just V8 + WebGPU — no filesystem, no network, no Node compat.

- **WebGPU via wgpu.** Deno's WebGPU implementation wraps `wgpu-core`. gpu_share forks `deno_webgpu` (from Deno v2.7.7) and adds `importSharedTexture()` to bridge wgpu's internal textures to Vulkan external memory.

## What's different from both

| Capability | Chrome / Deno | gpu_share |
|---|---|---|
| **GPU surface sharing** | Chrome uses mailbox textures (internal GPU abstraction). Deno doesn't share surfaces at all. | Vulkan `VK_EXTERNAL_MEMORY` — the renderer exports a file descriptor (Linux) or HANDLE (Windows) for its shared VkImage, and the presenter imports it directly. Zero-copy. |
| **Frame synchronization** | Chrome uses sync tokens (GPU-level fences managed by the command buffer). Deno doesn't need cross-process sync. | Exportable `VK_SEMAPHORE_TYPE_TIMELINE` — renderer signals a monotonic counter after each frame copy, presenter reads it before blit. No GPU-level wait in the presenter's command buffer, just a CPU-side poll. |
| **Handle passing** | Chrome passes GPU resources through Viz. Deno doesn't. | Raw OS handle passing: Linux `pidfd_open` + `pidfd_getfd` (kernel 5.6+), Windows `DuplicateHandle`. Handles sent as `uint64` in protobuf messages. |
| **Rendering API** | Chrome: Skia + ANGLE (abstracted). Deno: WebGPU in-process. | WebGPU scenes render to a headless VkImage, never to a window. The presenter does the only `vkQueuePresent`. |
| **Double-buffering** | Chrome composites multiple layers. Deno presents directly. | Renderer has two images: `render_img` (scene writes here) and `shared_img` (presenter reads here). After each frame, a GPU copy moves pixels from render to shared, so the shared image is only written for microseconds. |
