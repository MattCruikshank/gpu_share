# WebGPU Integration Plan

Replace the custom `op_gpu_*` ops in deno_renderer with the real WebGPU API via a minimal fork of `deno_webgpu`.

## Why fork deno_webgpu (not write ops from scratch)

- `deno_webgpu` is ~9,250 lines implementing ~150-200 WebGPU methods for deno_core
- Writing that from scratch would be weeks of work
- The fork changes are small (~80 lines across 2-3 files) because:
  - deno_webgpu already checks for a pre-existing Instance in OpState before creating one
  - wgpu-core already has all `from_hal` methods we need
  - deno_webgpu uses wgpu-core directly (not the wgpu wrapper), so HAL access is available

## Architecture

```
deno_renderer (Rust)
    |
    |-- ash: creates Vulkan instance + device with external memory extensions
    |-- wgpu-hal: wraps ash objects as HAL objects
    |-- wgpu-core: wraps HAL objects as wgpu Global/Adapter/Device
    |-- deno_webgpu (forked): registers WebGPU ops with deno_core
    |       |-- pre-populated OpState with our wgpu-core Instance
    |       |-- injected adapter + device with external memory extensions
    |       |-- new op: create_texture_from_hal for shared surface
    |
    V8 (deno_core)
        |-- scene.ts uses standard WebGPU API:
        |   navigator.gpu.requestAdapter() → our injected adapter
        |   adapter.requestDevice() → our injected device
        |   device.createShaderModule(), createRenderPipeline(), etc.
        |   renderPass.draw(), etc.
        |   Our shared surface exposed as a GPUTexture
```

## Step-by-step plan

### Step 1: Fork deno_webgpu into the repo

- Copy `ext/webgpu/` from denoland/deno into `deno_renderer/webgpu/`
- Adjust it to be a local module (not a separate crate — inline into our workspace)
- Pin wgpu-core version to match what deno_webgpu expects (28.x)
- Add wgpu-hal with vulkan feature as a dependency
- Verify it compiles standalone

### Step 2: wgpu-hal integration layer (~200-400 lines, new file)

Create `deno_renderer/src/wgpu_hal_bridge.rs` that:

1. **Wraps ash Instance**: `wgpu_hal::vulkan::Instance::from_raw(entry, raw_instance, ...)` → HAL instance
2. **Wraps physical device**: Create `wgpu_hal::vulkan::ExposedAdapter` from our chosen physical device
3. **Creates device with extensions**: Use `vulkan::Adapter::device_from_raw()` or `open_with_callback()` to create a device with `VK_KHR_external_memory_fd` / `VK_KHR_external_memory_win32` enabled
4. **Lifts to wgpu-core**:
   - `Global::from_hal_instance::<Vulkan>(hal_instance)` → Instance
   - `Global::create_adapter_from_hal(hal_adapter)` → AdapterId
   - `Global::create_device_from_hal(adapter_id, hal_device, desc)` → (DeviceId, QueueId)
5. **Wraps shared VkImage**: `vulkan::Device::texture_from_raw(vk_image, desc, drop_guard, TextureMemory::External)` → HAL texture, then `Global::create_texture_from_hal(hal_texture, device_id, desc)` → TextureId

Exports: a struct containing the Arc<Global>, AdapterId, DeviceId, QueueId, and TextureId.

### Step 3: Modify deno_webgpu fork (~80 lines)

**lib.rs — Adapter injection (~15 lines)**:
- In `request_adapter`, check if a pre-registered AdapterId exists in OpState
- If so, return it instead of enumerating adapters

**adapter.rs — Device injection (~20 lines)**:
- In `request_device`, check if a pre-registered DeviceId exists in OpState
- If so, return it instead of calling `adapter_request_device`

**device.rs — Texture from HAL (~30 lines)**:
- Add a new method or op `import_shared_texture` that takes the pre-registered TextureId and returns a GPUTexture object to JS

**No changes needed for Instance injection** — the existing code already uses a pre-populated Instance from OpState if present.

### Step 4: Wire into deno_renderer main.rs

Before creating the JsRuntime:

1. Create ash Vulkan objects (as we do now)
2. Call wgpu_hal_bridge to wrap them as wgpu-core objects
3. Create `Arc<Global>` (the wgpu-core Instance)
4. Register adapter, device, texture IDs

When creating the JsRuntime:

5. Register the forked deno_webgpu extension
6. Pre-populate OpState with the Arc<Global> Instance
7. Pre-populate OpState with injected AdapterId, DeviceId, TextureId

### Step 5: Update scene scripts

Old (custom ops):
```js
const shader = op_gpu_create_shader_module(wgslCode);
const pipeline = op_gpu_create_render_pipeline(shader, "vs", "fs");
op_gpu_draw(pipeline, 3, 1);
```

New (standard WebGPU):
```js
const adapter = await navigator.gpu.requestAdapter();
const device = await adapter.requestDevice();
const sharedTexture = device.importSharedTexture(); // our custom addition

const shaderModule = device.createShaderModule({ code: wgslCode });
const pipeline = device.createRenderPipeline({
  layout: "auto",
  vertex: { module: shaderModule, entryPoint: "vs" },
  fragment: { module: shaderModule, entryPoint: "fs", targets: [{ format: "rgba8unorm" }] },
});

// Each frame:
const encoder = device.createCommandEncoder();
const pass = encoder.beginRenderPass({
  colorAttachments: [{
    view: sharedTexture.createView(),
    loadOp: "clear",
    storeOp: "store",
    clearValue: { r: 0.1, g: 0.1, b: 0.1, a: 1 },
  }],
});
pass.setPipeline(pipeline);
pass.draw(3);
pass.end();
device.queue.submit([encoder.finish()]);
```

### Step 6: Keep or remove custom ops

Keep the existing custom ops alongside deno_webgpu during the transition. Scene scripts that use the old API continue to work. New scenes use WebGPU. Remove old ops once all scenes are migrated.

## Dependencies

```toml
# deno_renderer/Cargo.toml additions
wgpu-core = { version = "28", features = ["vulkan"] }
wgpu-hal = { version = "28", features = ["vulkan"] }
wgpu-types = "28"
```

Version 28 to match deno_webgpu 0.207.0's dependency. We may need to pin exact versions if there are minor incompatibilities.

## Risks

1. **wgpu-hal Vulkan internals** — The HAL bridge is the hardest part. `from_raw` / `texture_from_raw` are unsafe and require matching wgpu-hal's internal expectations exactly. If the struct layouts or assumptions change between wgpu-hal versions, this breaks.

2. **deno_webgpu update cadence** — deno_webgpu updates frequently with Deno releases. Our fork will drift. Mitigation: keep changes minimal and additive so rebasing is easy.

3. **deno_core version compatibility** — Our deno_core 0.393 must be compatible with the forked deno_webgpu's deno_core dependency. May need version alignment.

4. **Push constants** — WebGPU doesn't have push constants (our scenes use them via custom ops). Scenes would need to use uniform buffers instead, which is more verbose but standard.

5. **Render loop ownership** — Currently Rust drives the render loop and calls JS each frame. With WebGPU, JS may want to own the render loop via requestAnimationFrame. Need to decide who drives.

## Estimated effort

| Component | Lines | Difficulty |
|-----------|-------|------------|
| Fork + setup deno_webgpu | ~0 new code, config only | Easy |
| wgpu-hal bridge | ~200-400 | Hard |
| deno_webgpu modifications | ~80 | Easy |
| Wire into main.rs | ~100 | Moderate |
| Migrate scene scripts | ~200 per scene | Easy |
| **Total** | **~600-900** | **Moderate-Hard** |

The wgpu-hal bridge is where we'll spend most debugging time.
