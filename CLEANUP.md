# Cleanup TODO

Remaining code debt.

## Minor cleanup

### `presenter/grpc_server.cpp`
- Commented-out debug print (~line 172)
- Unused `#include <atomic>`

### `presenter/main.cpp`
- Unused `#include <thread>`

### `shared/shared_handle.h`
- Still used by presenter for `SharedSurfaceInfo` and `SharedMemoryHandle` types
- Low priority: could be replaced by protobuf struct

### `deno_renderer/scenes/2.ts`
- Remove `frameCount` / first-frame debug logging (added during WebGPU debugging)
