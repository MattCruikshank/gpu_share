# WebGPU Integration — Debug Status

## Current state (commit 8fa6cf4)

The code is in a **diagnostic mode**: real bridge is created, but scene script loading is skipped (`if false` around script load). This isolates whether the bridge itself breaks rendering.

### What works
- deno_webgpu fork compiles cleanly (forked from Deno v2.7.7, wgpu-core 28)
- wgpu-hal bridge creates successfully (wraps ash Instance/Device as wgpu-core objects)
- Shared texture imports via `create_texture_from_hal`
- Scene scripts load, set up WebGPU pipelines, and enter the render loop without crashing
- `webgpu_shim.js` bootstraps `navigator.gpu` + `GPUBufferUsage` in bare V8
- All 5 scenes migrated to standard WebGPU API (uniform buffers, command encoders, queue.submit)
- The OLD render path (`render_frame` → `record_clear_only`) still works when no JS touches the GPU

### What doesn't work
- **Black screen** when WebGPU scenes run and submit frames via `device.queue.submit()`
- Even a diagnostic magenta clear in `transition_for_presenter` showed black when scenes were active

### Root cause narrowed down

Through elimination testing:

| Test | Result | Conclusion |
|------|--------|------------|
| Real bridge + WebGPU scene + WebGPU render path | Black | — |
| Real bridge + WebGPU scene + forced OLD render path | Black | JS `queue.submit()` in `__frame` interferes |
| Real bridge + NO scene + OLD render path | **Visible gray** (needs retest after reboot) | Bridge alone doesn't break rendering |
| Dummy bridge + WebGPU scene | Crash: `Device[Id(0,1)] does not exist` | Confirms scene actually uses WebGPU device |

**The bridge itself is fine.** The black screen happens when the JS scene's `device.queue.submit()` (through wgpu-core) runs before our Vulkan `render_frame`/`transition_for_presenter`.

### Likely cause: wgpu-core queue submission breaks image state

When `__frame()` calls `device.queue.submit()`:
1. wgpu-core submits a command buffer with relay semaphores (binary semaphores for ordering)
2. wgpu-core's render pass transitions the shared image through internal layout states
3. Our subsequent `render_frame` or `transition_for_presenter` uses raw `vkQueueSubmit` — it doesn't participate in wgpu-core's relay semaphore chain
4. Either: wrong image layout assumed, or GPU execution ordering issue

### Next steps to try

1. **Retest the "bridge + no scene" case** to confirm gray is visible (was interrupted by CreateProcess error 4551)

2. **Test with a minimal scene that only clears** (no shader/pipeline, just `beginRenderPass` with a clear, `end`, `submit`). This isolates whether the issue is the shader validation error or the queue submission itself.

3. **Add `queue_wait_idle` AFTER `device.queue.submit` in JS** by wrapping the frame callback on the Rust side — call `queue_wait_idle` between `__frame()` and `transition_for_presenter()`. This ensures wgpu-core's GPU work is fully complete.

4. **Try using wgpu-core's API for the layout transition** instead of raw Vulkan. Call `Global::command_encoder_*` + `queue_submit` through wgpu-core to do the TRANSFER_SRC transition, keeping everything in wgpu-core's semaphore chain.

5. **Check if `createView()` each frame leaks** — the scene creates a new texture view every frame. If wgpu-core tracks these and transitions the texture, it might leave the image in an unexpected state.

### Files changed from upstream deno_webgpu

Only 3 files modified (~50 lines total):
- `webgpu/lib.rs` — Removed `deps = [deno_webidl, deno_web]` and ESM; added injection types (`InjectedAdapterId`, `InjectedDeviceIds`, `InjectedTexture`); adapter injection in `request_adapter`
- `webgpu/adapter.rs` — Device injection in `request_device`
- `webgpu/device.rs` — Added `importSharedTexture()` method on GPUDevice

### Key architecture

```
main.rs creates ash Vulkan objects (Instance, Device, SharedImage)
    → wgpu_hal_bridge::create_bridge() wraps them as wgpu-core Global/Adapter/Device
    → wgpu_hal_bridge::import_texture() wraps SharedImage as wgpu-core Texture
    → OpState populated with InjectedAdapterId, InjectedDeviceIds, InjectedTexture
    → Scene JS: navigator.gpu.requestAdapter() → injected adapter
    → Scene JS: adapter.requestDevice() → injected device
    → Scene JS: device.importSharedTexture() → shared VkImage as GPUTexture
    → Scene JS: standard WebGPU render loop (createCommandEncoder, beginRenderPass, draw, submit)
    → Rust render loop: transition shared image to TRANSFER_SRC_OPTIMAL for presenter
```

### Diagnostic code to remove

When the black screen is fixed, remove:
- `if false` block around scene script loading (main.rs ~line 1731)
- Magenta clear in `transition_for_presenter`
- Branch logging (`LOGGED_BRANCH` / `LOGGED_BRANCH2` statics)
- Frame error logging can stay (it's useful)
