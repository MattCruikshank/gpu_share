// Scene 4: Starfield — points flying toward the camera
// Mouse moves the vanishing point, scroll changes speed.

const {
  op_gpu_create_shader_module,
  op_gpu_create_render_pipeline,
  op_gpu_set_clear_color,
  op_gpu_draw,
  op_gpu_poll_events,
  op_gpu_set_rotation,
  op_log,
} = Deno.core.ops;

op_log("Scene 4: Starfield");

const shader = op_gpu_create_shader_module(`
  struct PushConstants {
    angle: f32,
    aspect_ratio: f32,
    scale: f32,
  };
  var<push_constant> pc: PushConstants;

  struct VsOutput {
    @builtin(position) pos: vec4f,
    @location(0) color: vec3f,
  };

  @vertex fn vs(@builtin(vertex_index) i: u32) -> VsOutput {
    // Fullscreen quad
    var positions = array<vec2f, 6>(
      vec2f(-1.0, -1.0), vec2f( 1.0, -1.0), vec2f( 1.0,  1.0),
      vec2f(-1.0, -1.0), vec2f( 1.0,  1.0), vec2f(-1.0,  1.0)
    );

    var out: VsOutput;
    out.pos = vec4f(positions[i], 0.0, 1.0);
    out.color = vec3f(positions[i] * 0.5 + 0.5, 0.0);
    return out;
  }

  // Hash function for pseudo-random stars
  fn hash(p: vec2f) -> f32 {
    let h = dot(p, vec2f(127.1, 311.7));
    return fract(sin(h) * 43758.5453);
  }

  @fragment fn fs(@location(0) color: vec3f) -> @location(0) vec4f {
    let uv = color.xy;
    let t = pc.angle;

    // Create a grid of "stars"
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
`);

if (shader === 0xFFFFFFFF) {
  op_log("ERROR: Failed to create shader module");
} else {
  const pipeline = op_gpu_create_render_pipeline(shader, "vs", "fs");
  if (pipeline === 0xFFFFFFFF) {
    op_log("ERROR: Failed to create render pipeline");
  } else {
    op_gpu_set_clear_color(0.0, 0.0, 0.02, 1.0);
    op_gpu_draw(pipeline, 6, 1);

    let speed = 1.0;

    globalThis.__frame = (elapsed) => {
      const rawBuf = op_gpu_poll_events();
      if (rawBuf.length > 0 && globalThis.proto) {
        const events = proto.decodeEvents(rawBuf);
        for (const ev of events) {
          if (ev.event && ev.event.case === "mouseWheel") {
            speed *= ev.event.value.scrollY > 0 ? 1.2 : 0.8;
            speed = Math.max(0.1, Math.min(10.0, speed));
          }
          if (ev.event && ev.event.case === "keyDown") {
            if (ev.event.value.scancode === 44) speed = speed === 0 ? 1.0 : 0;
          }
        }
      }
      op_gpu_set_rotation(elapsed * speed, 0, 1);
    };

    op_log("Starfield ready! scroll=speed, space=pause");
  }
}
