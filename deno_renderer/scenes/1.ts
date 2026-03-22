// Scene 1: Spinning RGB triangle — standard WebGPU API
// Interactive: mouse drag rotates, scroll zooms, space pauses rotation.
// Input events still use op_gpu_poll_events (protobuf from gRPC).

const { op_gpu_poll_events, op_gpu_set_webgpu_active, op_log } = Deno.core.ops;

op_log("Scene 1 (WebGPU): setting up...");

// Tell Rust we handle rendering — skip its render_frame()
op_gpu_set_webgpu_active(true);

// Async IIFE for top-level await (execute_script doesn't support bare await)
(async () => {

// ---------------------------------------------------------------------------
// WebGPU init
// ---------------------------------------------------------------------------
const adapter = await navigator.gpu.requestAdapter();
const device = await adapter.requestDevice();
let sharedTexture = device.importSharedTexture();

// ---------------------------------------------------------------------------
// Shader — uniform buffer replaces push constants
// ---------------------------------------------------------------------------
const shaderModule = device.createShaderModule({
  code: `
    struct Uniforms {
      angle: f32,
      aspect_ratio: f32,
      scale: f32,
    };
    @group(0) @binding(0) var<uniform> u: Uniforms;

    struct VsOutput {
      @builtin(position) pos: vec4f,
      @location(0) color: vec3f,
    };

    @vertex fn vs(@builtin(vertex_index) i: u32) -> VsOutput {
      const positions = array<vec2f, 3>(
        vec2f( 0.0, -0.5),
        vec2f( 0.5,  0.5),
        vec2f(-0.5,  0.5)
      );
      const colors = array<vec3f, 3>(
        vec3f(1.0, 0.0, 0.0),
        vec3f(0.0, 1.0, 0.0),
        vec3f(0.0, 0.0, 1.0),
      );

      let pos = positions[i];
      let s = sin(u.angle);
      let c = cos(u.angle);
      var rotated = vec2f(pos.x * c - pos.y * s, pos.x * s + pos.y * c);
      rotated *= u.scale;
      rotated.x /= u.aspect_ratio;

      return VsOutput(vec4f(rotated, 0.0, 1.0), colors[i]);
    }

    @fragment fn fs(@location(0) color: vec3f) -> @location(0) vec4f {
      return vec4f(color, 1.0);
    }
  `,
});

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------
const pipeline = device.createRenderPipeline({
  layout: "auto",
  vertex: {
    module: shaderModule,
    entryPoint: "vs",
  },
  fragment: {
    module: shaderModule,
    entryPoint: "fs",
    targets: [{ format: "rgba8unorm" }],
  },
  primitive: {
    topology: "triangle-list",
  },
});

// ---------------------------------------------------------------------------
// Uniform buffer (3 floats: angle, aspect_ratio, scale)
// ---------------------------------------------------------------------------
const uniformBuffer = device.createBuffer({
  size: 16, // 3 floats + padding to 16-byte alignment
  usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
});

const bindGroup = device.createBindGroup({
  layout: pipeline.getBindGroupLayout(0),
  entries: [{ binding: 0, resource: { buffer: uniformBuffer } }],
});

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
let dragAngle = 0;
let speed = 1.0;
let scale = 1.0;
let dragging = false;
let width = sharedTexture.width;
let height = sharedTexture.height;

// ---------------------------------------------------------------------------
// Per-frame callback (called by Rust render loop)
// ---------------------------------------------------------------------------
globalThis.__frame = (elapsed) => {
  // Poll input events
  const rawBuf = op_gpu_poll_events();
  if (rawBuf.length > 0 && globalThis.proto) {
    const events = proto.decodeEvents(rawBuf);
    for (const ev of events) {
      switch (ev.event?.case) {
        case "mouseButton":
          dragging = ev.event.value.pressed && ev.event.value.button === 1;
          break;
        case "mouseMotion":
          if (dragging) {
            dragAngle += ev.event.value.relX * 0.01;
          }
          break;
        case "mouseWheel":
          scale *= ev.event.value.scrollY > 0 ? 1.1 : 0.9;
          scale = Math.max(0.1, Math.min(10.0, scale));
          break;
        case "keyDown":
          if (ev.event.value.scancode === 44) {
            // space
            speed = speed === 0 ? 1.0 : 0;
          }
          break;
        case "resize":
          // Re-import shared texture at new size
          sharedTexture = device.importSharedTexture();
          width = sharedTexture.width;
          height = sharedTexture.height;
          break;
      }
    }
  }

  // Update uniform buffer
  const angle = elapsed * speed + dragAngle;
  const aspectRatio = width / height;
  const uniformData = new Float32Array([angle, aspectRatio, scale]);
  device.queue.writeBuffer(uniformBuffer, 0, uniformData);

  // Record and submit
  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({
    colorAttachments: [
      {
        view: sharedTexture.createView(),
        loadOp: "clear",
        storeOp: "store",
        clearValue: { r: 0.1, g: 0.1, b: 0.15, a: 1.0 },
      },
    ],
  });

  pass.setPipeline(pipeline);
  pass.setBindGroup(0, bindGroup);
  pass.draw(3);
  pass.end();

  device.queue.submit([encoder.finish()]);
};

op_log("Interactive scene ready! drag=rotate, scroll=zoom, space=pause");

})();
