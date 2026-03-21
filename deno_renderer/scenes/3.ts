// Scene 3: Pulsing concentric rings
// Animates radius and color based on elapsed time.

const {
  op_gpu_create_shader_module,
  op_gpu_create_render_pipeline,
  op_gpu_set_clear_color,
  op_gpu_draw,
  op_gpu_poll_events,
  op_gpu_set_rotation,
  op_log,
} = Deno.core.ops;

op_log("Scene 3: Pulsing rings");

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
    // Fullscreen quad (two triangles)
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
    // pc.angle = elapsed time, pc.scale = speed multiplier
    let uv = color.xy;
    let center = vec2f(0.5, 0.5);
    let d = distance(uv, center);

    let t = pc.angle * pc.scale;
    let ring = sin(d * 30.0 - t * 4.0) * 0.5 + 0.5;
    let pulse = sin(t * 2.0) * 0.3 + 0.7;

    let r = ring * (0.5 + 0.5 * sin(t));
    let g = ring * (0.5 + 0.5 * sin(t + 2.094));
    let b = ring * (0.5 + 0.5 * sin(t + 4.189));

    return vec4f(r * pulse, g * pulse, b * pulse, 1.0);
  }
`);

if (shader === 0xFFFFFFFF) {
  op_log("ERROR: Failed to create shader module");
} else {
  const pipeline = op_gpu_create_render_pipeline(shader, "vs", "fs");
  if (pipeline === 0xFFFFFFFF) {
    op_log("ERROR: Failed to create render pipeline");
  } else {
    op_gpu_set_clear_color(0.0, 0.0, 0.0, 1.0);
    op_gpu_draw(pipeline, 6, 1);

    let speed = 1.0;

    globalThis.__frame = (elapsed) => {
      const rawBuf = op_gpu_poll_events();
      if (rawBuf.length > 0 && globalThis.proto) {
        const events = proto.decodeEvents(rawBuf);
        for (const ev of events) {
          if (ev.event && ev.event.case === "keyDown") {
            if (ev.event.value.scancode === 44) speed = speed === 0 ? 1.0 : 0;
          }
          if (ev.event && ev.event.case === "mouseWheel") {
            speed *= ev.event.value.scrollY > 0 ? 1.2 : 0.8;
            speed = Math.max(0.1, Math.min(5.0, speed));
          }
        }
      }
      op_gpu_set_rotation(elapsed, 0, speed);
    };

    op_log("Pulsing rings ready! scroll=speed, space=pause");
  }
}
