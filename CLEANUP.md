# Cleanup TODO

Code debt accumulated during the gRPC migration and multi-tab implementation.

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
- Can be re-scaffolded if needed; remove to reduce clutter
- Also remove the `BUILD_RENDERER_EXTENSION` option from root `CMakeLists.txt`

## Remove from `deno_renderer/src/main.rs`

### Commented-out debug prints
- Line ~1128: `// eprintln!("[transport] StreamInput connected, waiting for events");`
- Line ~1131: `// eprintln!("[transport] Received event: case={:?}", ev.event);`
- Line ~1583: `// eprintln!("[render_loop] Got event: {:?}", ev.event);`

### Stale log label
- Line ~807: `eprintln!("[scene.ts] {}", msg)` — should be `[scene]` since scenes are no longer named `scene.ts`

### Unused dependency in Cargo.toml
- `serde = { version = "1", features = ["derive"] }` — not used anywhere in the crate

### `#[allow(dead_code)]` audit
- `BufferRes` struct (~line 63): exists but never instantiated — remove if not planned
- `GpuState` (~line 73): has the annotation but most fields ARE used — remove the annotation and suppress individually if needed
- `presenter_pid` field in `GrpcTransport` (~line 1057): stored but never read

## Remove from `presenter/grpc_server.cpp`

### Commented-out debug print
- Line ~172: `// fprintf(stderr, "[grpc_server] StreamInput: sending event ...`

### Unused include
- `#include <atomic>` — no `std::atomic` used in this file

## Remove from `presenter/main.cpp`

### Unused include
- `#include <thread>` — no `std::thread` used directly

## Consider

### `shared/shared_handle.h`
- Still used by presenter for `SharedSurfaceInfo` and `SharedMemoryHandle` types
- The protobuf `SharedSurfaceInfo` message duplicates the C++ struct
- Could be replaced by using the generated protobuf struct everywhere, but low priority since it's only used at the transport boundary

### `deno_renderer/scene.ts` (deleted)
- Already removed — was moved to `scenes/1.ts`. Verify no stale references remain.

### Diagnostic log in `v8_polyfill.js`
- Self-test at the bottom can be removed once TextEncoder polyfill is confirmed stable on all platforms
