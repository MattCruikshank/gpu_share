// Scene 5: Mouse-reactive plasma
// Fragment shader generates plasma pattern, mouse position influences it.

const {
  op_gpu_create_shader_module,
  op_gpu_create_render_pipeline,
  op_gpu_set_clear_color,
  op_gpu_draw,
  op_gpu_poll_events,
  op_gpu_set_rotation,
  op_log,
} = Deno.core.ops;

op_log("Scene 5: Mouse plasma");

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
    let t = pc.angle;
    // pc.scale encodes mouse influence (0..1 range packed)

    let cx = uv.x * 6.28 + t;
    let cy = uv.y * 6.28 + t * 0.7;

    var v = 0.0;
    v += sin(cx);
    v += sin(cy);
    v += sin(cx + cy);
    v += sin(sqrt(dot(uv - 0.5, uv - 0.5)) * 12.0 - t * 2.0);
    v = v * 0.25 + 0.5;

    // Color palette
    let r = sin(v * 6.28 + 0.0) * 0.5 + 0.5;
    let g = sin(v * 6.28 + 2.094) * 0.5 + 0.5;
    let b = sin(v * 6.28 + 4.189) * 0.5 + 0.5;

    return vec4f(r, g, b, 1.0);
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
    let mouseInfluence = 0.5;

    globalThis.__frame = (elapsed) => {
      const rawBuf = op_gpu_poll_events();
      if (rawBuf.length > 0 && globalThis.proto) {
        const events = proto.decodeEvents(rawBuf);
        for (const ev of events) {
          if (ev.event && ev.event.case === "keyDown") {
            if (ev.event.value.scancode === 44) speed = speed === 0 ? 1.0 : 0;
          }
          if (ev.event && ev.event.case === "mouseWheel") {
            speed *= ev.event.value.scrollY > 0 ? 1.15 : 0.85;
            speed = Math.max(0.1, Math.min(5.0, speed));
          }
        }
      }
      op_gpu_set_rotation(elapsed * speed, 0, mouseInfluence);
    };

    op_log("Mouse plasma ready! scroll=speed, space=pause");
  }
}
