// WebGPU shim for bare V8 (no deno_web/deno_webidl).
// Provides navigator.gpu and GPUBufferUsage/GPUTextureUsage/GPUShaderStage
// by calling the deno_webgpu ops directly.

(function() {
  const ops = Deno.core.ops;

  // Stub EventTarget infrastructure that op_create_gpu requires.
  // Our injected adapter/device bypass the JS EventTarget wrapping,
  // so these stubs just need to not crash.
  const brand = Symbol("webidl.brand");
  const setEventTargetData = function(_obj) {};
  const ErrorEvent = class extends Error {};

  // Create the GPU object via the native op
  const gpu = ops.op_create_gpu(brand, setEventTargetData, ErrorEvent);

  // Expose as navigator.gpu
  if (typeof navigator === "undefined") {
    globalThis.navigator = {};
  }
  navigator.gpu = gpu;

  // WebGPU usage flag constants
  globalThis.GPUBufferUsage = {
    MAP_READ:      0x0001,
    MAP_WRITE:     0x0002,
    COPY_SRC:      0x0004,
    COPY_DST:      0x0008,
    INDEX:         0x0010,
    VERTEX:        0x0020,
    UNIFORM:       0x0040,
    STORAGE:       0x0080,
    INDIRECT:      0x0100,
    QUERY_RESOLVE: 0x0200,
  };

  globalThis.GPUTextureUsage = {
    COPY_SRC:          0x01,
    COPY_DST:          0x02,
    TEXTURE_BINDING:   0x04,
    STORAGE_BINDING:   0x08,
    RENDER_ATTACHMENT: 0x10,
  };

  globalThis.GPUShaderStage = {
    VERTEX:   0x1,
    FRAGMENT: 0x2,
    COMPUTE:  0x4,
  };

  globalThis.GPUMapMode = {
    READ:  0x0001,
    WRITE: 0x0002,
  };
})();
