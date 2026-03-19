#include "shared_surface.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <cstdio>
#include <cstring>

#define VK_CHECK(call)                                                         \
    do {                                                                       \
        VkResult result_ = (call);                                             \
        if (result_ != VK_SUCCESS) {                                           \
            fprintf(stderr, "SharedSurface: %s failed with VkResult %d at "    \
                            "%s:%d\n",                                         \
                    #call, static_cast<int>(result_), __FILE__, __LINE__);     \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool SharedSurface::getGodotVulkanHandles() {
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    if (!rs) {
        fprintf(stderr, "SharedSurface: RenderingServer singleton not available\n");
        return false;
    }

    godot::RenderingDevice *rd = rs->get_rendering_device();
    if (!rd) {
        fprintf(stderr, "SharedSurface: RenderingDevice not available\n");
        return false;
    }

    device_ = reinterpret_cast<VkDevice>(
        rd->get_driver_resource(
            godot::RenderingDevice::DRIVER_RESOURCE_VULKAN_DEVICE,
            godot::RID(), 0));

    physDevice_ = reinterpret_cast<VkPhysicalDevice>(
        rd->get_driver_resource(
            godot::RenderingDevice::DRIVER_RESOURCE_VULKAN_PHYSICAL_DEVICE,
            godot::RID(), 0));

    if (device_ == VK_NULL_HANDLE || physDevice_ == VK_NULL_HANDLE) {
        fprintf(stderr, "SharedSurface: Failed to obtain Vulkan device handles from Godot\n");
        return false;
    }

    return true;
}

uint32_t SharedSurface::findMemoryType(uint32_t typeFilter,
                                       VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice_, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    fprintf(stderr, "SharedSurface: Failed to find suitable memory type\n");
    return UINT32_MAX;
}

bool SharedSurface::init(uint32_t width, uint32_t height, VkFormat format) {
    // Clean up any previous state
    destroy();

    width_ = width;
    height_ = height;
    format_ = format;

    if (!getGodotVulkanHandles()) {
        return false;
    }

    // Load extension function pointer
    pfnGetMemoryFdKHR_ = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    if (!pfnGetMemoryFdKHR_) {
        fprintf(stderr, "SharedSurface: vkGetMemoryFdKHR not available\n");
        return false;
    }

    // Create the exportable image
    VkExternalMemoryImageCreateInfo externalImageInfo{};
    externalImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalImageInfo.pNext = nullptr;
    externalImageInfo.handleTypes = kExternalMemoryHandleType;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &externalImageInfo;
    imageInfo.flags = 0;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format_;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.queueFamilyIndexCount = 0;
    imageInfo.pQueueFamilyIndices = nullptr;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VK_CHECK(vkCreateImage(device_, &imageInfo, nullptr, &image_));

    // Get memory requirements
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device_, image_, &memRequirements);
    memorySize_ = memRequirements.size;
    memoryTypeBits_ = memRequirements.memoryTypeBits;

    // Allocate exportable memory
    VkExportMemoryAllocateInfo exportAllocInfo{};
    exportAllocInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportAllocInfo.pNext = nullptr;
    exportAllocInfo.handleTypes = kExternalMemoryHandleType;

    uint32_t memTypeIndex = findMemoryType(
        memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memTypeIndex == UINT32_MAX) {
        fprintf(stderr, "SharedSurface: No suitable memory type found\n");
        return false;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &exportAllocInfo;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memTypeIndex;

    VK_CHECK(vkAllocateMemory(device_, &allocInfo, nullptr, &memory_));

    // Bind image to memory
    VK_CHECK(vkBindImageMemory(device_, image_, memory_, 0));

    fprintf(stderr, "SharedSurface: Initialized %ux%u exportable image "
                    "(memorySize=%llu)\n",
            width_, height_, static_cast<unsigned long long>(memorySize_));

    return true;
}

int SharedSurface::exportMemoryFd() {
    if (!pfnGetMemoryFdKHR_ || memory_ == VK_NULL_HANDLE) {
        fprintf(stderr, "SharedSurface: Cannot export fd - not initialized\n");
        return -1;
    }

    VkMemoryGetFdInfoKHR getFdInfo{};
    getFdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    getFdInfo.pNext = nullptr;
    getFdInfo.memory = memory_;
    getFdInfo.handleType = kExternalMemoryHandleType;

    int fd = -1;
    VkResult result = pfnGetMemoryFdKHR_(device_, &getFdInfo, &fd);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "SharedSurface: vkGetMemoryFdKHR failed with %d\n",
                static_cast<int>(result));
        return -1;
    }

    return fd;
}

SharedSurfaceInfo SharedSurface::getSurfaceInfo() const {
    SharedSurfaceInfo info;
    info.width = width_;
    info.height = height_;
    info.format = format_;
    info.memorySize = memorySize_;
    info.memoryTypeBits = memoryTypeBits_;
    return info;
}

void SharedSurface::destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    if (image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }

    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }

    memorySize_ = 0;
    memoryTypeBits_ = 0;
    width_ = 0;
    height_ = 0;
}
