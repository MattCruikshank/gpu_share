// Scene script for deno_renderer — WebGPU-like API
// TypeScript defines shaders, pipelines, and draw calls.
// Rust renders each frame to the shared exportable VkImage.

const {
  op_gpu_create_shader_module,
  op_gpu_create_render_pipeline,
  op_gpu_set_clear_color,
  op_gpu_draw,
  op_log,
} = Deno.core.ops;

op_log("Scene script loaded — setting up WebGPU pipeline");

// Create shader module with WGSL
const shader = op_gpu_create_shader_module(`
  @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    var pos = array<vec2f, 3>(
      vec2f( 0.0, -0.5),
      vec2f( 0.5,  0.5),
      vec2f(-0.5,  0.5)
    );
    return vec4f(pos[i], 0.0, 1.0);
  }

  @fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    // Gradient based on position
    let r = pos.x / 640.0;
    let g = pos.y / 480.0;
    return vec4f(r, g, 0.8, 1.0);
  }
`);

if (shader === 0xFFFFFFFF) {
  op_log("ERROR: Failed to create shader module");
} else {
  op_log(`Shader module created: id=${shader}`);

  // Create render pipeline
  const pipeline = op_gpu_create_render_pipeline(shader, "vs", "fs");

  if (pipeline === 0xFFFFFFFF) {
    op_log("ERROR: Failed to create render pipeline");
  } else {
    op_log(`Render pipeline created: id=${pipeline}`);

    // Set clear color (dark blue-gray)
    op_gpu_set_clear_color(0.1, 0.1, 0.15, 1.0);

    // Draw a triangle every frame (3 vertices, 1 instance)
    op_gpu_draw(pipeline, 3, 1);

    op_log("Scene ready: triangle with position-based gradient");
  }
}
