#pragma once

#include <cstdint>

enum class InputEventType : uint8_t {
    None = 0,
    MouseMotion,
    MouseButton,
    MouseWheel,
    KeyDown,
    KeyUp,
    Resize,
};

struct InputEvent {
    InputEventType type = InputEventType::None;
    uint8_t padding[3] = {};

    union {
        struct { float x, y, relX, relY; } motion;
        struct { uint8_t button; uint8_t pressed; float x, y; } mouseButton;
        struct { float scrollY; } wheel;
        struct { uint32_t scancode; } key;
        struct { uint32_t width, height; } resize;
    };
};

static_assert(sizeof(InputEvent) == 20, "InputEvent size must match Rust side (20 bytes)");
