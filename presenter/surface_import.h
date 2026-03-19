#pragma once
#include "../shared/shared_handle.h"
#include <vulkan/vulkan.h>

struct ImportedSurface {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    SharedSurfaceInfo info;

    void destroy(VkDevice device);
};

ImportedSurface importSurface(VkDevice device, VkPhysicalDevice physDevice,
                              SharedMemoryHandle memoryHandle, const SharedSurfaceInfo& info);
