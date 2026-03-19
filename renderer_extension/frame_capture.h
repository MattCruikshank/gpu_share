#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <vulkan/vulkan.h>
#include "shared_surface.h"

namespace godot {

class FrameCapture : public Node {
    GDCLASS(FrameCapture, Node)

public:
    FrameCapture();
    ~FrameCapture();

    void set_capture_size(int width, int height);
    int get_memory_fd();
    Dictionary get_surface_info();

    void _ready() override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    SharedSurface sharedSurface_;
    bool initialized_ = false;
    int captureWidth_ = 640;
    int captureHeight_ = 480;

    // Vulkan handles for blit operations
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;

    void initVulkanResources();
    void blitViewportToSharedSurface();
};

} // namespace godot
