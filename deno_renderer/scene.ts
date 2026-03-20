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
    var out: VsOutput;
    out.pos = vec4f(positions[i], 0.0, 1.0);
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

    op_log("Scene ready: RGB triangle via WGSL!");
  }
}
