// Test renderer: headless Vulkan process that creates an exportable VkImage,
// renders a test pattern, and sends the fd to the presenter via Unix socket.

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <csignal>
    #include <unistd.h>
#endif

#include "shared/shared_handle.h"
#include "handle_transport/handle_transport.h"

#define VK_CHECK(call) do { \
    VkResult result_ = (call); \
    if (result_ != VK_SUCCESS) { \
        fprintf(stderr, "Vulkan error %d at %s:%d\n", result_, __FILE__, __LINE__); \
        abort(); \
    } \
} while(0)

// ---------------------------------------------------------------------------
// Globals for signal handler cleanup
// ---------------------------------------------------------------------------
#ifdef _WIN32
static volatile LONG g_running = 1;
#else
static volatile sig_atomic_t g_running = 1;
#endif

static VkInstance       g_instance      = VK_NULL_HANDLE;
static VkDevice         g_device        = VK_NULL_HANDLE;
static VkImage          g_image         = VK_NULL_HANDLE;
static VkDeviceMemory   g_memory        = VK_NULL_HANDLE;
static VkCommandPool    g_commandPool   = VK_NULL_HANDLE;
static SharedMemoryHandle g_memoryHandle = kInvalidMemoryHandle;

#ifdef _WIN32
static BOOL WINAPI consoleHandler(DWORD) {
    g_running = 0;
    return TRUE;
}
#else
static void signalHandler(int) {
    g_running = 0;
}
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t findMemoryType(VkPhysicalDevice physDev,
                               uint32_t memoryTypeBits,
                               VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & required) == required) {
            return i;
        }
    }
    fprintf(stderr, "Failed to find suitable memory type\n");
    abort();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Parse socket path from command line
#ifdef _WIN32
    std::string socketPath = "gpu-share";
#else
    std::string socketPath = "/tmp/gpu-share.sock";
#endif
    for (int i = 1; i < argc; ++i) {
        if ((std::string(argv[i]) == "--socket" || std::string(argv[i]) == "-s") && i + 1 < argc) {
            socketPath = argv[++i];
        }
    }

    // Install signal/ctrl handler
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif

    // -----------------------------------------------------------------------
    // 1. Vulkan instance (headless, no surface extensions)
    // -----------------------------------------------------------------------
    fprintf(stderr, "[test_renderer] Creating Vulkan instance (headless)...\n");

    // Check for validation layer
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    bool hasValidation = false;
    const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
    for (const auto& layer : availableLayers) {
        if (strcmp(layer.layerName, validationLayerName) == 0) {
            hasValidation = true;
            break;
        }
    }

    std::vector<const char*> instanceLayers;
    if (hasValidation) {
        instanceLayers.push_back(validationLayerName);
        fprintf(stderr, "[test_renderer] Enabling validation layer\n");
    }

    std::vector<const char*> instanceExtensions = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pNext              = nullptr;
    appInfo.pApplicationName   = "test_renderer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName        = "none";
    appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceCI{};
    instanceCI.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCI.pNext                   = nullptr;
    instanceCI.flags                   = 0;
    instanceCI.pApplicationInfo        = &appInfo;
    instanceCI.enabledLayerCount       = static_cast<uint32_t>(instanceLayers.size());
    instanceCI.ppEnabledLayerNames     = instanceLayers.empty() ? nullptr : instanceLayers.data();
    instanceCI.enabledExtensionCount   = static_cast<uint32_t>(instanceExtensions.size());
    instanceCI.ppEnabledExtensionNames = instanceExtensions.data();

    VK_CHECK(vkCreateInstance(&instanceCI, nullptr, &g_instance));
    fprintf(stderr, "[test_renderer] VkInstance created\n");

    // -----------------------------------------------------------------------
    // 2. Pick physical device with a graphics queue family
    // -----------------------------------------------------------------------
    uint32_t physDevCount = 0;
    vkEnumeratePhysicalDevices(g_instance, &physDevCount, nullptr);
    if (physDevCount == 0) {
        fprintf(stderr, "No Vulkan physical devices found\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> physDevices(physDevCount);
    vkEnumeratePhysicalDevices(g_instance, &physDevCount, physDevices.data());

    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily = UINT32_MAX;

    for (const auto& pd : physDevices) {
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfProps.data());

        for (uint32_t i = 0; i < qfCount; ++i) {
            if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physDev = pd;
                graphicsQueueFamily = i;
                break;
            }
        }
        if (physDev != VK_NULL_HANDLE) break;
    }

    if (physDev == VK_NULL_HANDLE) {
        fprintf(stderr, "No physical device with graphics queue found\n");
        return 1;
    }

    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physDev, &props);
        fprintf(stderr, "[test_renderer] Using device: %s\n", props.deviceName);
    }

    // -----------------------------------------------------------------------
    // 3. Create logical device
    // -----------------------------------------------------------------------
    float queuePriority = 1.0f;

    VkDeviceQueueCreateInfo queueCI{};
    queueCI.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCI.pNext            = nullptr;
    queueCI.flags            = 0;
    queueCI.queueFamilyIndex = graphicsQueueFamily;
    queueCI.queueCount       = 1;
    queueCI.pQueuePriorities = &queuePriority;

    std::vector<const char*> deviceExtensions = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#else
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
#endif
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };

    VkDeviceCreateInfo deviceCI{};
    deviceCI.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCI.pNext                   = nullptr;
    deviceCI.flags                   = 0;
    deviceCI.queueCreateInfoCount    = 1;
    deviceCI.pQueueCreateInfos       = &queueCI;
    deviceCI.enabledLayerCount       = 0;
    deviceCI.ppEnabledLayerNames     = nullptr;
    deviceCI.enabledExtensionCount   = static_cast<uint32_t>(deviceExtensions.size());
    deviceCI.ppEnabledExtensionNames = deviceExtensions.data();
    deviceCI.pEnabledFeatures        = nullptr;

    VK_CHECK(vkCreateDevice(physDev, &deviceCI, nullptr, &g_device));
    fprintf(stderr, "[test_renderer] VkDevice created\n");

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(g_device, graphicsQueueFamily, 0, &graphicsQueue);

    // -----------------------------------------------------------------------
    // 4. Command pool
    // -----------------------------------------------------------------------
    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.pNext            = nullptr;
    poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = graphicsQueueFamily;

    VK_CHECK(vkCreateCommandPool(g_device, &poolCI, nullptr, &g_commandPool));

    // -----------------------------------------------------------------------
    // 5. Create exportable shared image (640x480, R8G8B8A8_UNORM)
    // -----------------------------------------------------------------------
    const uint32_t imageWidth  = 640;
    const uint32_t imageHeight = 480;
    const VkFormat imageFormat = VK_FORMAT_R8G8B8A8_UNORM;

    fprintf(stderr, "[test_renderer] Creating exportable image %ux%u...\n", imageWidth, imageHeight);

    VkExternalMemoryImageCreateInfo extMemImageCI{};
    extMemImageCI.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemImageCI.pNext       = nullptr;
    extMemImageCI.handleTypes = kExternalMemoryHandleType;

    VkImageCreateInfo imageCI{};
    imageCI.sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCI.pNext                 = &extMemImageCI;
    imageCI.flags                 = 0;
    imageCI.imageType             = VK_IMAGE_TYPE_2D;
    imageCI.format                = imageFormat;
    imageCI.extent                = { imageWidth, imageHeight, 1 };
    imageCI.mipLevels             = 1;
    imageCI.arrayLayers           = 1;
    imageCI.samples               = VK_SAMPLE_COUNT_1_BIT;
    imageCI.tiling                = VK_IMAGE_TILING_OPTIMAL;
    imageCI.usage                 = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCI.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.queueFamilyIndexCount = 0;
    imageCI.pQueueFamilyIndices   = nullptr;
    imageCI.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;

    VK_CHECK(vkCreateImage(g_device, &imageCI, nullptr, &g_image));

    // Get memory requirements
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(g_device, g_image, &memReqs);

    fprintf(stderr, "[test_renderer] Memory requirements: size=%llu, alignment=%llu, typeBits=0x%x\n",
            (unsigned long long)memReqs.size,
            (unsigned long long)memReqs.alignment,
            memReqs.memoryTypeBits);

    // Allocate exportable memory
    VkExportMemoryAllocateInfo exportAllocInfo{};
    exportAllocInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportAllocInfo.pNext       = nullptr;
    exportAllocInfo.handleTypes = kExternalMemoryHandleType;

    uint32_t memTypeIndex = findMemoryType(physDev, memReqs.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext           = &exportAllocInfo;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = memTypeIndex;

    VK_CHECK(vkAllocateMemory(g_device, &allocInfo, nullptr, &g_memory));
    VK_CHECK(vkBindImageMemory(g_device, g_image, g_memory, 0));
    fprintf(stderr, "[test_renderer] Image created and memory bound\n");

    // Export the memory handle (only once -- each call creates a new handle)
#ifdef _WIN32
    auto pfnGetMemoryWin32HandleKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
        vkGetDeviceProcAddr(g_device, "vkGetMemoryWin32HandleKHR"));
    if (!pfnGetMemoryWin32HandleKHR) {
        fprintf(stderr, "Failed to load vkGetMemoryWin32HandleKHR\n");
        return 1;
    }

    VkMemoryGetWin32HandleInfoKHR getHandleInfo{};
    getHandleInfo.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
    getHandleInfo.memory     = g_memory;
    getHandleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VK_CHECK(pfnGetMemoryWin32HandleKHR(g_device, &getHandleInfo, &g_memoryHandle));
    fprintf(stderr, "[test_renderer] Exported memory handle\n");
#else
    auto pfnGetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(g_device, "vkGetMemoryFdKHR"));
    if (!pfnGetMemoryFdKHR) {
        fprintf(stderr, "Failed to load vkGetMemoryFdKHR\n");
        return 1;
    }

    VkMemoryGetFdInfoKHR getFdInfo{};
    getFdInfo.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    getFdInfo.pNext      = nullptr;
    getFdInfo.memory     = g_memory;
    getFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;

    VK_CHECK(pfnGetMemoryFdKHR(g_device, &getFdInfo, &g_memoryHandle));
    fprintf(stderr, "[test_renderer] Exported memory fd = %d\n", g_memoryHandle);
#endif

    // -----------------------------------------------------------------------
    // 6. Render test pattern (cornflower blue clear)
    // -----------------------------------------------------------------------
    fprintf(stderr, "[test_renderer] Rendering test pattern...\n");

    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.pNext              = nullptr;
    cmdAllocInfo.commandPool        = g_commandPool;
    cmdAllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(g_device, &cmdAllocInfo, &cmdBuf));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.pNext            = nullptr;
    beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = nullptr;

    VK_CHECK(vkBeginCommandBuffer(cmdBuf, &beginInfo));

    // Transition: UNDEFINED -> TRANSFER_DST_OPTIMAL
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.pNext                           = nullptr;
        barrier.srcAccessMask                   = 0;
        barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = g_image;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;

        vkCmdPipelineBarrier(cmdBuf,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);
    }

    // Clear to cornflower blue (0.39, 0.58, 0.93, 1.0)
    VkClearColorValue clearColor = {{ 0.39f, 0.58f, 0.93f, 1.0f }};
    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel   = 0;
    clearRange.levelCount     = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount     = 1;

    vkCmdClearColorImage(cmdBuf, g_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearColor, 1, &clearRange);

    // Transition: TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.pNext                           = nullptr;
        barrier.srcAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = g_image;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;

        vkCmdPipelineBarrier(cmdBuf,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);
    }

    VK_CHECK(vkEndCommandBuffer(cmdBuf));

    // Submit and wait with fence
    VkFenceCreateInfo fenceCI{};
    fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCI.pNext = nullptr;
    fenceCI.flags = 0;

    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(g_device, &fenceCI, nullptr, &fence));

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = nullptr;
    submitInfo.waitSemaphoreCount   = 0;
    submitInfo.pWaitSemaphores      = nullptr;
    submitInfo.pWaitDstStageMask    = nullptr;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &cmdBuf;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores    = nullptr;

    VK_CHECK(vkQueueSubmit(graphicsQueue, 1, &submitInfo, fence));
    VK_CHECK(vkWaitForFences(g_device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(g_device, fence, nullptr);
    vkFreeCommandBuffers(g_device, g_commandPool, 1, &cmdBuf);
    fprintf(stderr, "[test_renderer] Test pattern rendered\n");

    // -----------------------------------------------------------------------
    // 7. Send fd to presenter via Unix domain socket
    // -----------------------------------------------------------------------
    auto transport = HandleTransport::create();
    if (!transport->listen(socketPath)) {
        fprintf(stderr, "Failed to listen on %s\n", socketPath.c_str());
        return 1;
    }

    fprintf(stderr, "[test_renderer] Waiting for presenter to connect on %s...\n", socketPath.c_str());

    if (!transport->accept()) {
        fprintf(stderr, "Failed to accept connection\n");
        return 1;
    }
    fprintf(stderr, "[test_renderer] Presenter connected\n");

    SharedSurfaceInfo surfaceInfo{};
    surfaceInfo.width          = imageWidth;
    surfaceInfo.height         = imageHeight;
    surfaceInfo.format         = imageFormat;
    surfaceInfo.memorySize     = memReqs.size;
    surfaceInfo.memoryTypeBits = memReqs.memoryTypeBits;

    if (!transport->sendHandle(g_memoryHandle, &surfaceInfo, sizeof(surfaceInfo))) {
        fprintf(stderr, "Failed to send handle\n");
        return 1;
    }
    fprintf(stderr, "[test_renderer] Surface shared, handle sent. Press Ctrl+C to exit.\n");

    // -----------------------------------------------------------------------
    // 8. Keep running so Vulkan resources stay alive
    // -----------------------------------------------------------------------
    while (g_running) {
#ifdef _WIN32
        Sleep(100);
#else
        pause();  // sleep until signal
#endif
    }

    fprintf(stderr, "\n[test_renderer] Shutting down...\n");

    // -----------------------------------------------------------------------
    // 9. Cleanup
    // -----------------------------------------------------------------------
    transport->close();

#ifdef _WIN32
    if (g_memoryHandle != kInvalidMemoryHandle) {
        CloseHandle(g_memoryHandle);
        g_memoryHandle = kInvalidMemoryHandle;
    }
#else
    if (g_memoryHandle != kInvalidMemoryHandle) {
        ::close(g_memoryHandle);
        g_memoryHandle = kInvalidMemoryHandle;
    }
#endif

    vkDestroyCommandPool(g_device, g_commandPool, nullptr);
    vkDestroyImage(g_device, g_image, nullptr);
    vkFreeMemory(g_device, g_memory, nullptr);
    vkDestroyDevice(g_device, nullptr);
    vkDestroyInstance(g_instance, nullptr);

    fprintf(stderr, "[test_renderer] Cleanup complete\n");
    return 0;
}
