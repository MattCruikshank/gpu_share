// Scene script for deno_renderer — WebGPU-like API
// TypeScript defines shaders, pipelines, and draw calls.
// Rust renders each frame to the shared exportable VkImage.
// Per-frame callback enables interactive input handling.
//
// Input events arrive as raw protobuf bytes from the gRPC stream.
// proto_bundle.js (loaded before this script) provides globalThis.proto
// with decodeEvents() and InputEventSchema.

const {
  op_gpu_create_shader_module,
  op_gpu_create_render_pipeline,
  op_gpu_set_clear_color,
  op_gpu_draw,
  op_gpu_poll_events,
  op_gpu_set_rotation,
  op_log,
} = Deno.core.ops;

// proto is set by proto_bundle.js on globalThis
// (globalThis.proto.decodeEvents, globalThis.proto.InputEventSchema)

op_log("Scene script loaded — setting up WebGPU pipeline");

// Create shader module with WGSL
// Push constants: float angle (offset 0), float aspectRatio (offset 4), float scale (offset 8)
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
    var positions = array<vec2f, 3>(
      vec2f( 0.0, -0.5),
      vec2f( 0.5,  0.5),
      vec2f(-0.5,  0.5)
    );
    var colors = array<vec3f, 3>(
      vec3f(1.0, 0.0, 0.0),
      vec3f(0.0, 1.0, 0.0),
      vec3f(0.0, 0.0, 1.0),
    );

    let pos = positions[i];
    let s = sin(pc.angle);
    let c = cos(pc.angle);
    var rotated = vec2f(pos.x * c - pos.y * s, pos.x * s + pos.y * c);
    rotated *= pc.scale;
    rotated.x /= pc.aspect_ratio;

    var out: VsOutput;
    out.pos = vec4f(rotated, 0.0, 1.0);
    out.color = colors[i];
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
    op_gpu_set_clear_color(0.1, 0.1, 0.15, 1.0);
    op_gpu_draw(pipeline, 3, 1);

    let dragAngle = 0;
    let speed = 1.0;
    let scale = 1.0;
    let dragging = false;

    globalThis.__frame = (elapsed) => {
      // op_gpu_poll_events returns length-prefixed protobuf bytes
      const rawBuf = op_gpu_poll_events();
      const events = proto.decodeEvents(rawBuf);

      for (const ev of events) {
        // ev.event is a oneof — ev.event.case tells you which variant
        switch (ev.event.case) {
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
            if (ev.event.value.scancode === 44) { // space
              speed = speed === 0 ? 1.0 : 0;
            }
            break;
        }
      }

      op_gpu_set_rotation(dragAngle, speed, scale);
    };

    op_log("Interactive scene ready! drag=rotate, scroll=zoom, space=pause");
  }
}
