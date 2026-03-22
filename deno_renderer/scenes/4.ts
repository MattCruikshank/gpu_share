// Scene 4: Starfield — points flying toward the camera — standard WebGPU API
// scroll=speed, space=pause

const { op_gpu_poll_events, op_gpu_set_webgpu_active, op_log } = Deno.core.ops;

op_log("Scene 4 (WebGPU): Starfield");
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

    fn hash(p: vec2f) -> f32 {
      let h = dot(p, vec2f(127.1, 311.7));
      return fract(sin(h) * 43758.5453);
    }

    @fragment fn fs(@location(0) color: vec3f) -> @location(0) vec4f {
      let uv = color.xy;
      let t = u.time;

      var brightness = 0.0;
      for (var layer = 0u; layer < 3u; layer++) {
        let speed = (f32(layer) + 1.0) * 0.5;
        let scale = 20.0 + f32(layer) * 15.0;
        let offset = vec2f(0.0, t * speed);
        let grid = fract(uv * scale + offset);
        let id = floor(uv * scale + offset);

        let rand = hash(id);
        let star_pos = vec2f(rand, hash(id + 1.0));
        let d = distance(grid, star_pos);

        let twinkle = sin(t * 3.0 + rand * 6.28) * 0.3 + 0.7;
        let size = 0.02 + rand * 0.03;
        let star = smoothstep(size, 0.0, d) * twinkle;

        let depth = 1.0 - f32(layer) * 0.3;
        brightness += star * depth;
      }

      let c = vec3f(0.8, 0.85, 1.0) * brightness;
      return vec4f(c, 1.0);
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
      if (ev.event?.case === "mouseWheel") {
        speed *= ev.event.value.scrollY > 0 ? 1.2 : 0.8;
        speed = Math.max(0.1, Math.min(10.0, speed));
      }
      if (ev.event?.case === "keyDown" && ev.event.value.scancode === 44) {
        speed = speed === 0 ? 1.0 : 0;
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
      clearValue: { r: 0, g: 0, b: 0.02, a: 1 },
    }],
  });
  pass.setPipeline(pipeline);
  pass.setBindGroup(0, bindGroup);
  pass.draw(6);
  pass.end();
  device.queue.submit([encoder.finish()]);
};

op_log("Starfield ready! scroll=speed, space=pause");

})();
