// Scene 5: Mouse-reactive plasma — standard WebGPU API
// Fragment shader generates plasma pattern. scroll=speed, space=pause

const { op_gpu_poll_events, op_gpu_set_webgpu_active, op_log } = Deno.core.ops;

op_log("Scene 5 (WebGPU): Mouse plasma");
op_gpu_set_webgpu_active(true);

(async () => {

const adapter = await navigator.gpu.requestAdapter();
const device = await adapter.requestDevice();
let sharedTexture = device.importSharedTexture();

const shaderModule = device.createShaderModule({
  code: `
    struct Uniforms {
      time: f32,
      _pad0: f32,
      _pad1: f32,
      _pad2: f32,
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
      let t = u.time;

      let cx = uv.x * 6.28 + t;
      let cy = uv.y * 6.28 + t * 0.7;

      var v = 0.0;
      v += sin(cx);
      v += sin(cy);
      v += sin(cx + cy);
      v += sin(sqrt(dot(uv - 0.5, uv - 0.5)) * 12.0 - t * 2.0);
      v = v * 0.25 + 0.5;

      let r = sin(v * 6.28 + 0.0) * 0.5 + 0.5;
      let g = sin(v * 6.28 + 2.094) * 0.5 + 0.5;
      let b = sin(v * 6.28 + 4.189) * 0.5 + 0.5;

      return vec4f(r, g, b, 1.0);
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
let accumTime = 0;
let lastElapsed = 0;
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
        speed *= ev.event.value.scrollY > 0 ? 1.15 : 0.85;
        speed = Math.max(0.1, Math.min(5.0, speed));
      }
      if (ev.event?.case === "resize") {
        sharedTexture = device.importSharedTexture();
        width = sharedTexture.width;
        height = sharedTexture.height;
      }
    }
  }

  const dt = lastElapsed > 0 ? elapsed - lastElapsed : 0.016;
  lastElapsed = elapsed;
  accumTime += dt * speed;

  device.queue.writeBuffer(uniformBuffer, 0, new Float32Array([accumTime, 0, 0, 0]));

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

op_log("Mouse plasma ready! scroll=speed, space=pause");

})();
