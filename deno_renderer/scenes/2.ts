// Scene 2: Bouncing octagon — standard WebGPU API
// Spacebar pauses the bounce.

const { op_gpu_poll_events, op_gpu_set_webgpu_active, op_log } = Deno.core.ops;

op_log("Scene 2 (WebGPU): Bouncing octagon");

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
// Shader — uniform buffer with position, aspect ratio
// ---------------------------------------------------------------------------
const shaderModule = device.createShaderModule({
  code: `
    struct Uniforms {
      offset_x: f32,
      offset_y: f32,
      aspect_ratio: f32,
      _pad: f32,
    };
    @group(0) @binding(0) var<uniform> u: Uniforms;

    struct VsOutput {
      @builtin(position) pos: vec4f,
      @location(0) color: vec3f,
    };

    @vertex fn vs(@builtin(vertex_index) i: u32) -> VsOutput {
      // 8 triangles forming an octagon (fan from center)
      // 24 vertices: for each triangle, center + two edge points
      let NUM_SIDES: u32 = 8u;
      let tri = i / 3u;
      let vert = i % 3u;

      let angle_step = 6.283185 / f32(NUM_SIDES);
      var p: vec2f;
      if (vert == 0u) {
        p = vec2f(0.0, 0.0); // center
      } else if (vert == 1u) {
        let a = angle_step * f32(tri);
        p = vec2f(cos(a), sin(a)) * 0.4;
      } else {
        let a = angle_step * f32(tri + 1u);
        p = vec2f(cos(a), sin(a)) * 0.4;
      }

      p.x = p.x / u.aspect_ratio + u.offset_x;
      p.y = p.y + u.offset_y;

      // Color: warm gradient based on triangle index
      let t = f32(tri) / f32(NUM_SIDES);
      let color = vec3f(
        0.9 - t * 0.4,
        0.3 + t * 0.5,
        0.2 + t * 0.3
      );

      return VsOutput(vec4f(p, 0.0, 1.0), color);
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
// Uniform buffer (4 floats: offset_x, offset_y, aspect_ratio, _pad)
// ---------------------------------------------------------------------------
const uniformBuffer = device.createBuffer({
  size: 16,
  usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
});

const bindGroup = device.createBindGroup({
  layout: pipeline.getBindGroupLayout(0),
  entries: [{ binding: 0, resource: { buffer: uniformBuffer } }],
});

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
let x = 0.0;
let y = 0.0;
let vx = 0.6;
let vy = 0.45;
let paused = false;
let lastTime = 0;
let width = sharedTexture.width;
let height = sharedTexture.height;

// ---------------------------------------------------------------------------
// Per-frame callback
// ---------------------------------------------------------------------------
let frameCount = 0;
globalThis.__frame = (elapsed) => {
  frameCount++;
  if (frameCount === 1) op_log("[scene2] First __frame call");
  // Process input events
  const rawBuf = op_gpu_poll_events();
  if (rawBuf.length > 0 && globalThis.proto) {
    const events = proto.decodeEvents(rawBuf);
    for (const ev of events) {
      if (ev.event?.case === "keyDown") {
        if (ev.event.value.scancode === 44) {
          // space
          paused = !paused;
        }
      }
      if (ev.event?.case === "resize") {
        sharedTexture = device.importSharedTexture();
        width = sharedTexture.width;
        height = sharedTexture.height;
      }
    }
  }

  // Update bounce
  const dt = lastTime > 0 ? elapsed - lastTime : 0.016;
  lastTime = elapsed;

  if (!paused) {
    x += vx * dt;
    y += vy * dt;

    if (x > 0.55 || x < -0.55) {
      vx = -vx;
      x = Math.max(-0.55, Math.min(0.55, x));
    }
    if (y > 0.55 || y < -0.55) {
      vy = -vy;
      y = Math.max(-0.55, Math.min(0.55, y));
    }
  }

  // Update uniform buffer
  const aspectRatio = width / height;
  const uniformData = new Float32Array([x, y, aspectRatio, 0]);
  device.queue.writeBuffer(uniformBuffer, 0, uniformData);

  // Record and submit
  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({
    colorAttachments: [
      {
        view: sharedTexture.createView(),
        loadOp: "clear",
        storeOp: "store",
        clearValue: { r: 0.05, g: 0.05, b: 0.1, a: 1.0 },
      },
    ],
  });

  pass.setPipeline(pipeline);
  pass.setBindGroup(0, bindGroup);
  pass.draw(24); // 8 triangles × 3 vertices
  pass.end();

  device.queue.submit([encoder.finish()]);
};

op_log("Bouncing octagon ready! space=pause");

})();
