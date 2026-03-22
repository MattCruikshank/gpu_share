// Scene 3: Pulsing concentric rings — standard WebGPU API
// Animates radius and color based on elapsed time.
// scroll=speed, space=pause

const { op_gpu_poll_events, op_gpu_set_webgpu_active, op_log } = Deno.core.ops;

op_log("Scene 3 (WebGPU): Pulsing rings");
op_gpu_set_webgpu_active(true);

(async () => {

const adapter = await navigator.gpu.requestAdapter();
const device = await adapter.requestDevice();
let sharedTexture = device.importSharedTexture();

const shaderModule = device.createShaderModule({
  code: `
    struct Uniforms {
      time: f32,
      speed: f32,
      _pad0: f32,
      _pad1: f32,
    };
    @group(0) @binding(0) var<uniform> u: Uniforms;

    struct VsOutput {
      @builtin(position) pos: vec4f,
      @location(0) color: vec3f,
    };

    @vertex fn vs(@builtin(vertex_index) i: u32) -> VsOutput {
      var positions = array<vec2f, 6>(
        vec2f(-1.0, -1.0), vec2f( 1.0, -1.0), vec2f( 1.0,  1.0),
        vec2f(-1.0, -1.0), vec2f( 1.0,  1.0), vec2f(-1.0,  1.0)
      );

      var out: VsOutput;
      out.pos = vec4f(positions[i], 0.0, 1.0);
      out.color = vec3f(positions[i] * 0.5 + 0.5, 0.0);
      return out;
    }

    @fragment fn fs(@location(0) color: vec3f) -> @location(0) vec4f {
      let uv = color.xy;
      let center = vec2f(0.5, 0.5);
      let d = distance(uv, center);

      let t = u.time * u.speed;
      let ring = sin(d * 30.0 - t * 4.0) * 0.5 + 0.5;
      let pulse = sin(t * 2.0) * 0.3 + 0.7;

      let r = ring * (0.5 + 0.5 * sin(t));
      let g = ring * (0.5 + 0.5 * sin(t + 2.094));
      let b = ring * (0.5 + 0.5 * sin(t + 4.189));

      return vec4f(r * pulse, g * pulse, b * pulse, 1.0);
    }
  `,
});

const pipeline = device.createRenderPipeline({
  layout: "auto",
  vertex: { module: shaderModule, entryPoint: "vs" },
  fragment: {
    module: shaderModule,
    entryPoint: "fs",
    targets: [{ format: "rgba8unorm" }],
  },
  primitive: { topology: "triangle-list" },
});

const uniformBuffer = device.createBuffer({
  size: 16,
  usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
});

const bindGroup = device.createBindGroup({
  layout: pipeline.getBindGroupLayout(0),
  entries: [{ binding: 0, resource: { buffer: uniformBuffer } }],
});

let speed = 1.0;
let width = sharedTexture.width;
let height = sharedTexture.height;

globalThis.__frame = (elapsed) => {
  const rawBuf = op_gpu_poll_events();
  if (rawBuf.length > 0 && globalThis.proto) {
    const events = proto.decodeEvents(rawBuf);
    for (const ev of events) {
      if (ev.event?.case === "keyDown" && ev.event.value.scancode === 44) {
        speed = speed === 0 ? 1.0 : 0;
      }
      if (ev.event?.case === "mouseWheel") {
        speed *= ev.event.value.scrollY > 0 ? 1.2 : 0.8;
        speed = Math.max(0.1, Math.min(5.0, speed));
      }
      if (ev.event?.case === "resize") {
        sharedTexture = device.importSharedTexture();
        width = sharedTexture.width;
        height = sharedTexture.height;
      }
    }
  }

  device.queue.writeBuffer(uniformBuffer, 0, new Float32Array([elapsed, speed, 0, 0]));

  const encoder = device.createCommandEncoder();
  const pass = encoder.beginRenderPass({
    colorAttachments: [{
      view: sharedTexture.createView(),
      loadOp: "clear",
      storeOp: "store",
      clearValue: { r: 0, g: 0, b: 0, a: 1 },
    }],
  });
  pass.setPipeline(pipeline);
  pass.setBindGroup(0, bindGroup);
  pass.draw(6);
  pass.end();
  device.queue.submit([encoder.finish()]);
};

op_log("Pulsing rings ready! scroll=speed, space=pause");

})();
