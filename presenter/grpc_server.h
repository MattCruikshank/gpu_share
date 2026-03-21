#pragma once

#include "../shared/shared_handle.h"
#include <cstdint>
#include <memory>

// Opaque implementation — hides gRPC headers from presenter/main.cpp
struct GrpcBridgeImpl;

class GrpcBridge {
public:
    GrpcBridge();
    ~GrpcBridge();

    // Start the gRPC server on a background thread. Returns false on failure.
    bool start(uint16_t port);

    // Stop the server and join the background thread.
    void stop();

    // Block until a renderer calls Connect. Returns false on timeout/shutdown.
    bool waitForRenderer(SharedMemoryHandle& outHandle,
                         SharedSurfaceInfo& outInfo,
                         uint32_t timeoutMs = 30000);

    // Push an input event to be streamed to the renderer.
    // eventType values match InputEventType enum.
    // data/dataLen is the union payload bytes.
    void pushMouseMotion(float x, float y, float relX, float relY);
    void pushMouseButton(uint32_t button, bool pressed, float x, float y);
    void pushMouseWheel(float scrollY);
    void pushKeyDown(uint32_t scancode);
    void pushKeyUp(uint32_t scancode);
    void pushResize(uint32_t width, uint32_t height);

    // Poll for surface updates from the renderer (after resize).
    // Returns true if a new surface is available.
    bool pollSurfaceUpdate(SharedMemoryHandle& outHandle,
                           SharedSurfaceInfo& outInfo);

private:
    std::unique_ptr<GrpcBridgeImpl> impl_;
};
