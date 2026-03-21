// Scene 2: Bouncing octagon with spacebar pause
// Uses push constants for position + scale, renders an octagon via 8 triangles.

const {
  op_gpu_create_shader_module,
  op_gpu_create_render_pipeline,
  op_gpu_set_clear_color,
  op_gpu_draw,
  op_gpu_poll_events,
  op_gpu_set_rotation,
  op_log,
} = Deno.core.ops;

op_log("Scene 2: Bouncing octagon");

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

    // Apply bounce position via push constants:
    // angle = encoded X position (-1..1 range, used as bounce X)
    // scale = encoded Y position (-1..1 range, used as bounce Y)
    // aspect_ratio = actual aspect ratio
    p.x = p.x / pc.aspect_ratio + pc.angle;
    p.y = p.y + pc.scale;

    // Color: warm gradient based on triangle index
    let t = f32(tri) / f32(NUM_SIDES);
    let color = vec3f(
      0.9 - t * 0.4,
      0.3 + t * 0.5,
      0.2 + t * 0.3
    );

    var out: VsOutput;
    out.pos = vec4f(p, 0.0, 1.0);
    out.color = color;
    return out;
  }

  @fragment fn fs(@location(0) color: vec3f) -> @location(0) vec4f {
    return vec4f(color, 1.0);
  }
`);

if (shader === 0xFFFFFFFF) {
  op_log("ERROR: Failed to create shader module");
} else {
  const pipeline = op_gpu_create_render_pipeline(shader, "vs", "fs");
  if (pipeline === 0xFFFFFFFF) {
    op_log("ERROR: Failed to create render pipeline");
  } else {
    op_gpu_set_clear_color(0.05, 0.05, 0.1, 1.0);
    // 8 triangles × 3 vertices = 24 vertices
    op_gpu_draw(pipeline, 24, 1);

    // Bounce state
    let x = 0.0;
    let y = 0.0;
    let vx = 0.6;  // velocity in NDC/sec
    let vy = 0.45;
    let paused = false;
    let lastTime = 0;

    globalThis.__frame = (elapsed) => {
      // Process input events
      const rawBuf = op_gpu_poll_events();
      if (rawBuf.length > 0 && globalThis.proto) {
        const events = proto.decodeEvents(rawBuf);
        for (const ev of events) {
          if (ev.event && ev.event.case === "keyDown") {
            if (ev.event.value.scancode === 44) { // space
              paused = !paused;
            }
          }
        }
      }

      // Update bounce
      const dt = lastTime > 0 ? elapsed - lastTime : 0.016;
      lastTime = elapsed;

      if (!paused) {
        x += vx * dt;
        y += vy * dt;

        // Bounce off edges (octagon radius ~0.4, screen edge at ±1)
        if (x > 0.55 || x < -0.55) {
          vx = -vx;
          x = Math.max(-0.55, Math.min(0.55, x));
        }
        if (y > 0.55 || y < -0.55) {
          vy = -vy;
          y = Math.max(-0.55, Math.min(0.55, y));
        }
      }

      // Abuse push constants: angle=x position, scale=y position
      // (rotation speed not used for this scene)
      op_gpu_set_rotation(x, 0, y);
    };

    op_log("Bouncing octagon ready! space=pause");
  }
}
