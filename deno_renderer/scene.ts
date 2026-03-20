// Scene script for deno_renderer
// This runs inside the embedded Deno runtime

const { op_set_clear_color, op_set_rotation_speed, op_log } = Deno.core.ops;

op_log("Scene script loaded!");

// Set a nice teal clear color
op_set_clear_color(0.0, 0.5, 0.6);

// Set rotation speed (will be used once we add geometry)
op_set_rotation_speed(1.5);

op_log("Scene configured: teal background, rotation speed 1.5 rad/s");
