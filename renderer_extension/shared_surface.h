#pragma once

#include <godot_cpp/classes/object.hpp>
#include <vulkan/vulkan.h>
#include "../shared/shared_handle.h"

// Manages a VkImage backed by exportable memory for cross-process GPU sharing.
// Uses raw Vulkan calls via handles obtained from Godot's RenderingDevice.
class SharedSurface {
public:
    bool init(uint32_t width, uint32_t height, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
    void destroy();

    // Export the memory fd (Linux). Each call returns a NEW fd.
    int exportMemoryFd();

    SharedSurfaceInfo getSurfaceInfo() const;
    VkImage getImage() const { return image_; }
    VkFormat getFormat() const { return format_; }
    uint32_t getWidth() const { return width_; }
    uint32_t getHeight() const { return height_; }

private:
    // Vulkan handles obtained from Godot's RenderingDevice
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice_ = VK_NULL_HANDLE;

    // Our exportable image
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    VkFormat format_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkDeviceSize memorySize_ = 0;
    uint32_t memoryTypeBits_ = 0;

    // Extension function pointers
    PFN_vkGetMemoryFdKHR pfnGetMemoryFdKHR_ = nullptr;

    bool getGodotVulkanHandles();
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
};
