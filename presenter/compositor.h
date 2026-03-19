#pragma once
#include <vulkan/vulkan.h>

class Compositor {
public:
    void recordBlit(VkCommandBuffer cmd,
                    VkImage srcImage, uint32_t srcWidth, uint32_t srcHeight,
                    VkImage dstImage, uint32_t dstWidth, uint32_t dstHeight);

    void recordClear(VkCommandBuffer cmd, VkImage dstImage,
                     uint32_t dstWidth, uint32_t dstHeight);
};
