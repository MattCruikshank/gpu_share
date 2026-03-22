# Cleanup TODO

Code debt accumulated during gRPC migration, multi-tab, and WebGPU integration.

## Delete entirely

### `test_renderer/`
- C++ test renderer, no longer used by the multi-tab presenter
- The presenter now only spawns deno_renderer processes
- Also remove `add_subdirectory(test_renderer)` from root `CMakeLists.txt`
- ~1000 lines of C++

### `handle_transport/`
- Old raw TCP transport, replaced by gRPC
- `handle_transport.h` and `handle_transport_tcp.cpp`
- Not compiled into any target; no references from active code
- ~650 lines

### `shared/input_event.h`
- Old C ABI `InputEvent` struct with tagged union
- Replaced by protobuf `InputEvent` with typed `oneof` in `proto/gpu_share.proto`
- No longer included by any active code

### `renderer_extension/`
- Godot GDExtension scaffolding, never integrated
- Disabled by default (`BUILD_RENDERER_EXTENSION=OFF`)
- Also remove the `BUILD_RENDERER_EXTENSION` option from root `CMakeLists.txt`

## Remove legacy custom ops from `deno_renderer/src/main.rs`

All scenes now use the standard WebGPU API. The old custom ops and their supporting types can be removed:

### Custom ops (no longer called by any scene)
- `op_gpu_create_shader_module` — replaced by `device.createShaderModule()`
- `op_gpu_create_render_pipeline` — replaced by `device.createRenderPipeline()`
- `op_gpu_create_buffer` — replaced by `device.createBuffer()`
- `op_gpu_set_clear_color` — replaced by render pass `clearValue`
- `op_gpu_draw`, `op_gpu_draw_with_vb` — replaced by `pass.draw()`
- `op_gpu_clear_draws` — no longer needed
- `op_gpu_set_rotation` — replaced by uniform buffers

### Supporting types (only used by custom ops)
- `ShaderModuleRes`, `RenderPipelineRes`, `BufferRes` structs
- `DrawState`, `DrawCommand` structs
- `GpuState` resource tables: `shader_modules`, `render_pipelines`, `buffers`, `draw_state`
- `render_frame()` function — only used by old render path
- `record_clear_only()` function — only used by `render_frame()`
- `submit_and_wait()` function — only used by `render_frame()`
- `create_image_view()`, `create_framebuffer()` — only used by old pipeline path
- Raw Vulkan `cmd_buf` and `fence` in the render loop — only needed for old path

### Other main.rs cleanup
- Commented-out debug prints
- `op_log` label says `[scene.ts]` — should be `[scene]`
- `serde` dependency in Cargo.toml — not used anywhere

## Minor cleanup

### `presenter/grpc_server.cpp`
- Commented-out debug print (~line 172)
- Unused `#include <atomic>`

### `presenter/main.cpp`
- Unused `#include <thread>`

### `shared/shared_handle.h`
- Still used by presenter for `SharedSurfaceInfo` and `SharedMemoryHandle` types
- Low priority: could be replaced by protobuf struct
