#include "frame_capture.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdio>

namespace godot {

FrameCapture::FrameCapture() = default;

FrameCapture::~FrameCapture() {
    if (device_ != VK_NULL_HANDLE) {
        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
            commandBuffer_ = VK_NULL_HANDLE; // freed with pool
        }
    }
    sharedSurface_.destroy();
}

void FrameCapture::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_capture_size", "width", "height"),
                         &FrameCapture::set_capture_size);
    ClassDB::bind_method(D_METHOD("get_memory_fd"),
                         &FrameCapture::get_memory_fd);
    ClassDB::bind_method(D_METHOD("get_surface_info"),
                         &FrameCapture::get_surface_info);
}

void FrameCapture::_ready() {
    initVulkanResources();

    if (!sharedSurface_.init(captureWidth_, captureHeight_)) {
        fprintf(stderr, "FrameCapture: Failed to initialize shared surface\n");
        return;
    }

    initialized_ = true;
    fprintf(stderr, "FrameCapture: Ready (%dx%d)\n", captureWidth_, captureHeight_);
}

void FrameCapture::initVulkanResources() {
    RenderingServer *rs = RenderingServer::get_singleton();
    if (!rs) {
        fprintf(stderr, "FrameCapture: RenderingServer not available\n");
        return;
    }

    RenderingDevice *rd = rs->get_rendering_device();
    if (!rd) {
        fprintf(stderr, "FrameCapture: RenderingDevice not available\n");
        return;
    }

    device_ = reinterpret_cast<VkDevice>(
        rd->get_driver_resource(
            RenderingDevice::DRIVER_RESOURCE_VULKAN_DEVICE,
            RID(), 0));

    queue_ = reinterpret_cast<VkQueue>(
        rd->get_driver_resource(
            RenderingDevice::DRIVER_RESOURCE_VULKAN_QUEUE,
            RID(), 0));

    queueFamily_ = static_cast<uint32_t>(
        rd->get_driver_resource(
            RenderingDevice::DRIVER_RESOURCE_VULKAN_QUEUE_FAMILY_INDEX,
            RID(), 0));

    if (device_ == VK_NULL_HANDLE || queue_ == VK_NULL_HANDLE) {
        fprintf(stderr, "FrameCapture: Failed to obtain Vulkan handles\n");
        return;
    }

    // Create command pool
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.pNext = nullptr;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;

    VkResult result = vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "FrameCapture: Failed to create command pool (%d)\n",
                static_cast<int>(result));
        return;
    }

    // Allocate command buffer
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.pNext = nullptr;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    result = vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer_);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "FrameCapture: Failed to allocate command buffer (%d)\n",
                static_cast<int>(result));
        return;
    }

    fprintf(stderr, "FrameCapture: Vulkan resources initialized "
                    "(queueFamily=%u)\n", queueFamily_);
}

void FrameCapture::_process(double delta) {
    if (!initialized_) {
        return;
    }

    blitViewportToSharedSurface();
}

void FrameCapture::blitViewportToSharedSurface() {
    if (commandBuffer_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
        return;
    }

    RenderingServer *rs = RenderingServer::get_singleton();
    RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (!rd) {
        return;
    }

    // Get the viewport texture's native VkImage handle.
    // In Godot 4.3+:
    //   Viewport::get_texture() -> ViewportTexture (RID via get_rid())
    //   RenderingServer::texture_get_rd_texture(viewport_tex_rid) -> RD texture RID
    //   RenderingDevice::texture_get_native_handle(rd_tex_rid) -> uint64_t (VkImage)
    Viewport *vp = get_viewport();
    if (!vp) {
        return;
    }

    RID viewportTexRid = vp->get_texture()->get_rid();
    RID rdTexRid = rs->texture_get_rd_texture(viewportTexRid);

    // TODO: texture_get_native_handle may not be available in all Godot 4.x
    // builds. If unavailable, this will need a fallback approach.
    uint64_t nativeHandle = rd->texture_get_native_handle(rdTexRid);
    if (nativeHandle == 0) {
        // Fallback: clear the shared surface to indicate no capture
        // This path is hit when the native handle API is not available
        return;
    }

    VkImage viewportImage = reinterpret_cast<VkImage>(nativeHandle);
    VkImage sharedImage = sharedSurface_.getImage();

    // Reset and begin command buffer
    vkResetCommandBuffer(commandBuffer_, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.pNext = nullptr;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = nullptr;

    VkResult result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
    if (result != VK_SUCCESS) {
        return;
    }

    // Transition viewport image: current layout -> TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier srcBarrier{};
    srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBarrier.pNext = nullptr;
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.image = viewportImage;
    srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBarrier.subresourceRange.baseMipLevel = 0;
    srcBarrier.subresourceRange.levelCount = 1;
    srcBarrier.subresourceRange.baseArrayLayer = 0;
    srcBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(commandBuffer_,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

    // Transition shared surface image: UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier dstBarrier{};
    dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBarrier.pNext = nullptr;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.image = sharedImage;
    dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dstBarrier.subresourceRange.baseMipLevel = 0;
    dstBarrier.subresourceRange.levelCount = 1;
    dstBarrier.subresourceRange.baseArrayLayer = 0;
    dstBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(commandBuffer_,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

    // Blit viewport image to shared surface image
    VkImageBlit blitRegion{};
    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.mipLevel = 0;
    blitRegion.srcSubresource.baseArrayLayer = 0;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.srcOffsets[0] = {0, 0, 0};
    blitRegion.srcOffsets[1] = {
        static_cast<int32_t>(captureWidth_),
        static_cast<int32_t>(captureHeight_),
        1};
    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.mipLevel = 0;
    blitRegion.dstSubresource.baseArrayLayer = 0;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.dstOffsets[0] = {0, 0, 0};
    blitRegion.dstOffsets[1] = {
        static_cast<int32_t>(sharedSurface_.getWidth()),
        static_cast<int32_t>(sharedSurface_.getHeight()),
        1};

    vkCmdBlitImage(commandBuffer_,
                   viewportImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   sharedImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blitRegion, VK_FILTER_LINEAR);

    // Transition shared surface image: TRANSFER_DST -> TRANSFER_SRC_OPTIMAL
    // (ready for the external presenter to read)
    VkImageMemoryBarrier finalBarrier{};
    finalBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    finalBarrier.pNext = nullptr;
    finalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    finalBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    finalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    finalBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    finalBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalBarrier.image = sharedImage;
    finalBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    finalBarrier.subresourceRange.baseMipLevel = 0;
    finalBarrier.subresourceRange.levelCount = 1;
    finalBarrier.subresourceRange.baseArrayLayer = 0;
    finalBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(commandBuffer_,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &finalBarrier);

    // Transition viewport image back to SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier restoreBarrier{};
    restoreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    restoreBarrier.pNext = nullptr;
    restoreBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    restoreBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    restoreBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restoreBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    restoreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreBarrier.image = viewportImage;
    restoreBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    restoreBarrier.subresourceRange.baseMipLevel = 0;
    restoreBarrier.subresourceRange.levelCount = 1;
    restoreBarrier.subresourceRange.baseArrayLayer = 0;
    restoreBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(commandBuffer_,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &restoreBarrier);

    vkEndCommandBuffer(commandBuffer_);

    // Submit and wait (Phase 1: synchronous. Later switch to semaphores.)
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = nullptr;
    submitInfo.waitSemaphoreCount = 0;
    submitInfo.pWaitSemaphores = nullptr;
    submitInfo.pWaitDstStageMask = nullptr;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer_;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores = nullptr;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = nullptr;
    fenceInfo.flags = 0;

    VkFence fence;
    result = vkCreateFence(device_, &fenceInfo, nullptr, &fence);
    if (result != VK_SUCCESS) {
        return;
    }

    result = vkQueueSubmit(queue_, 1, &submitInfo, fence);
    if (result != VK_SUCCESS) {
        vkDestroyFence(device_, fence, nullptr);
        return;
    }

    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
}

int FrameCapture::get_memory_fd() {
    return sharedSurface_.exportMemoryFd();
}

Dictionary FrameCapture::get_surface_info() {
    Dictionary info;
    SharedSurfaceInfo si = sharedSurface_.getSurfaceInfo();
    info["width"] = si.width;
    info["height"] = si.height;
    info["format"] = static_cast<int>(si.format);
    info["memory_size"] = static_cast<int64_t>(si.memorySize);
    info["memory_type_bits"] = static_cast<int>(si.memoryTypeBits);
    return info;
}

void FrameCapture::set_capture_size(int width, int height) {
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "FrameCapture: Invalid capture size %dx%d\n", width, height);
        return;
    }

    captureWidth_ = width;
    captureHeight_ = height;

    if (initialized_) {
        // Reinitialize with new size
        sharedSurface_.destroy();
        if (!sharedSurface_.init(captureWidth_, captureHeight_)) {
            fprintf(stderr, "FrameCapture: Failed to reinitialize shared surface "
                            "at %dx%d\n", captureWidth_, captureHeight_);
            initialized_ = false;
        }
    }
}

} // namespace godot
