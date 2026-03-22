#include "surface_import.h"
#include "vulkan_context.h"
#include <cstdio>

void ImportedSurface::destroy(VkDevice device) {
    if (timelineSem != VK_NULL_HANDLE) {
        vkDestroySemaphore(device, timelineSem, nullptr);
        timelineSem = VK_NULL_HANDLE;
    }
    if (imageView) vkDestroyImageView(device, imageView, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    imageView = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

static uint32_t findMemoryType(VkPhysicalDevice physDevice,
                               uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    fprintf(stderr, "Failed to find suitable memory type\n");
    std::abort();
}

ImportedSurface importSurface(VkDevice device, VkPhysicalDevice physDevice,
                              SharedMemoryHandle memoryHandle, const SharedSurfaceInfo& info) {
    ImportedSurface surface;
    surface.info = info;

    // Create image with external memory info
    VkExternalMemoryImageCreateInfo extMemImageInfo{};
    extMemImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemImageInfo.handleTypes = kExternalMemoryHandleType;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext = &extMemImageInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = info.format;
    imageInfo.extent = { info.width, info.height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VK_CHECK(vkCreateImage(device, &imageInfo, nullptr, &surface.image));

    // Get memory requirements
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, surface.image, &memReqs);

    // Import memory from handle
    // Note: On Windows, VK_USE_PLATFORM_WIN32_KHR must be defined before
    // including vulkan.h (handled via CMakeLists target_compile_definitions).
#ifdef _WIN32
    VkImportMemoryWin32HandleInfoKHR importWin32Info{};
    importWin32Info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    importWin32Info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    importWin32Info.handle = memoryHandle;
#else
    VkImportMemoryFdInfoKHR importFdInfo{};
    importFdInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
    importFdInfo.fd = memoryHandle;
#endif

    // Use the intersection of what the image needs and what the exported memory provides
    uint32_t memTypeBits = memReqs.memoryTypeBits & info.memoryTypeBits;
    if (memTypeBits == 0) {
        // Fall back to exported memory type bits if intersection is empty
        memTypeBits = info.memoryTypeBits;
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
#ifdef _WIN32
    allocInfo.pNext = &importWin32Info;
#else
    allocInfo.pNext = &importFdInfo;
#endif
    allocInfo.allocationSize = info.memorySize;
    allocInfo.memoryTypeIndex = findMemoryType(physDevice, memTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &surface.memory));
    // handle is now owned by Vulkan -- do not close it

    VK_CHECK(vkBindImageMemory(device, surface.image, surface.memory, 0));

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = surface.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = info.format;
    viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &surface.imageView));

    fprintf(stderr, "Imported surface: %ux%u, format=%d, memSize=%llu\n",
            info.width, info.height, static_cast<int>(info.format),
            static_cast<unsigned long long>(info.memorySize));

    return surface;
}

VkSemaphore importSemaphore(VkDevice device, VkPhysicalDevice /*physDevice*/,
                            SharedSemaphoreHandle handle) {
    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &typeInfo;

    VkSemaphore sem;
    VK_CHECK(vkCreateSemaphore(device, &semInfo, nullptr, &sem));

#ifdef _WIN32
    VkImportSemaphoreWin32HandleInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    importInfo.semaphore = sem;
    importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    importInfo.handle = handle;
    auto fpImport = (PFN_vkImportSemaphoreWin32HandleKHR)
        vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR");
    VK_CHECK(fpImport(device, &importInfo));
#else
    VkImportSemaphoreFdInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    importInfo.semaphore = sem;
    importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    importInfo.fd = handle;
    auto fpImport = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR");
    VK_CHECK(fpImport(device, &importInfo));
#endif

    fprintf(stderr, "[surface_import] Imported timeline semaphore\n");
    return sem;
}
