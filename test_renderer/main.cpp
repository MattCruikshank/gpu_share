// Test renderer: headless Vulkan process that creates an exportable VkImage,
// renders a spinning RGB triangle using a real graphics pipeline, and sends
// the fd/handle to the presenter via Unix socket / named pipe.

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>

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
// Embedded SPIR-V shaders (compiled from GLSL with glslangValidator)
// ---------------------------------------------------------------------------

// Vertex shader: rotating triangle with push constants (angle, aspectRatio)
static const uint32_t vertShaderSpirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000062, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000021, 0x00000055, 0x0000005d,
    0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005,
    0x0000000c, 0x69736f70, 0x6e6f6974, 0x00000073, 0x00040005, 0x00000017, 0x6f6c6f63, 0x00007372,
    0x00030005, 0x0000001e, 0x00736f70, 0x00060005, 0x00000021, 0x565f6c67, 0x65747265, 0x646e4978,
    0x00007865, 0x00030005, 0x00000027, 0x00000073, 0x00060005, 0x00000028, 0x68737550, 0x736e6f43,
    0x746e6174, 0x00000073, 0x00050006, 0x00000028, 0x00000000, 0x6c676e61, 0x00000065, 0x00060006,
    0x00000028, 0x00000001, 0x65707361, 0x61527463, 0x006f6974, 0x00030005, 0x0000002a, 0x00006370,
    0x00030005, 0x00000030, 0x00000063, 0x00040005, 0x00000034, 0x61746f72, 0x00646574, 0x00060005,
    0x00000053, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x00000053, 0x00000000,
    0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x00000053, 0x00000001, 0x505f6c67, 0x746e696f,
    0x657a6953, 0x00000000, 0x00070006, 0x00000053, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x00000053, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e,
    0x00030005, 0x00000055, 0x00000000, 0x00050005, 0x0000005d, 0x67617266, 0x6f6c6f43, 0x00000072,
    0x00040047, 0x00000021, 0x0000000b, 0x0000002a, 0x00030047, 0x00000028, 0x00000002, 0x00050048,
    0x00000028, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000028, 0x00000001, 0x00000023,
    0x00000004, 0x00030047, 0x00000053, 0x00000002, 0x00050048, 0x00000053, 0x00000000, 0x0000000b,
    0x00000000, 0x00050048, 0x00000053, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x00000053,
    0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x00000053, 0x00000003, 0x0000000b, 0x00000004,
    0x00040047, 0x0000005d, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002,
    0x00040015, 0x00000008, 0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000003,
    0x0004001c, 0x0000000a, 0x00000007, 0x00000009, 0x00040020, 0x0000000b, 0x00000006, 0x0000000a,
    0x0004003b, 0x0000000b, 0x0000000c, 0x00000006, 0x0004002b, 0x00000006, 0x0000000d, 0x00000000,
    0x0004002b, 0x00000006, 0x0000000e, 0xbf000000, 0x0005002c, 0x00000007, 0x0000000f, 0x0000000d,
    0x0000000e, 0x0004002b, 0x00000006, 0x00000010, 0x3f000000, 0x0005002c, 0x00000007, 0x00000011,
    0x00000010, 0x00000010, 0x0005002c, 0x00000007, 0x00000012, 0x0000000e, 0x00000010, 0x0006002c,
    0x0000000a, 0x00000013, 0x0000000f, 0x00000011, 0x00000012, 0x00040017, 0x00000014, 0x00000006,
    0x00000003, 0x0004001c, 0x00000015, 0x00000014, 0x00000009, 0x00040020, 0x00000016, 0x00000006,
    0x00000015, 0x0004003b, 0x00000016, 0x00000017, 0x00000006, 0x0004002b, 0x00000006, 0x00000018,
    0x3f800000, 0x0006002c, 0x00000014, 0x00000019, 0x00000018, 0x0000000d, 0x0000000d, 0x0006002c,
    0x00000014, 0x0000001a, 0x0000000d, 0x00000018, 0x0000000d, 0x0006002c, 0x00000014, 0x0000001b,
    0x0000000d, 0x0000000d, 0x00000018, 0x0006002c, 0x00000015, 0x0000001c, 0x00000019, 0x0000001a,
    0x0000001b, 0x00040020, 0x0000001d, 0x00000007, 0x00000007, 0x00040015, 0x0000001f, 0x00000020,
    0x00000001, 0x00040020, 0x00000020, 0x00000001, 0x0000001f, 0x0004003b, 0x00000020, 0x00000021,
    0x00000001, 0x00040020, 0x00000023, 0x00000006, 0x00000007, 0x00040020, 0x00000026, 0x00000007,
    0x00000006, 0x0004001e, 0x00000028, 0x00000006, 0x00000006, 0x00040020, 0x00000029, 0x00000009,
    0x00000028, 0x0004003b, 0x00000029, 0x0000002a, 0x00000009, 0x0004002b, 0x0000001f, 0x0000002b,
    0x00000000, 0x00040020, 0x0000002c, 0x00000009, 0x00000006, 0x0004002b, 0x00000008, 0x00000035,
    0x00000000, 0x0004002b, 0x00000008, 0x0000003a, 0x00000001, 0x0004002b, 0x0000001f, 0x0000004a,
    0x00000001, 0x00040017, 0x00000051, 0x00000006, 0x00000004, 0x0004001c, 0x00000052, 0x00000006,
    0x0000003a, 0x0006001e, 0x00000053, 0x00000051, 0x00000006, 0x00000052, 0x00000052, 0x00040020,
    0x00000054, 0x00000003, 0x00000053, 0x0004003b, 0x00000054, 0x00000055, 0x00000003, 0x00040020,
    0x0000005a, 0x00000003, 0x00000051, 0x00040020, 0x0000005c, 0x00000003, 0x00000014, 0x0004003b,
    0x0000005c, 0x0000005d, 0x00000003, 0x00040020, 0x0000005f, 0x00000006, 0x00000014, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x0000001d,
    0x0000001e, 0x00000007, 0x0004003b, 0x00000026, 0x00000027, 0x00000007, 0x0004003b, 0x00000026,
    0x00000030, 0x00000007, 0x0004003b, 0x0000001d, 0x00000034, 0x00000007, 0x0003003e, 0x0000000c,
    0x00000013, 0x0003003e, 0x00000017, 0x0000001c, 0x0004003d, 0x0000001f, 0x00000022, 0x00000021,
    0x00050041, 0x00000023, 0x00000024, 0x0000000c, 0x00000022, 0x0004003d, 0x00000007, 0x00000025,
    0x00000024, 0x0003003e, 0x0000001e, 0x00000025, 0x00050041, 0x0000002c, 0x0000002d, 0x0000002a,
    0x0000002b, 0x0004003d, 0x00000006, 0x0000002e, 0x0000002d, 0x0006000c, 0x00000006, 0x0000002f,
    0x00000001, 0x0000000d, 0x0000002e, 0x0003003e, 0x00000027, 0x0000002f, 0x00050041, 0x0000002c,
    0x00000031, 0x0000002a, 0x0000002b, 0x0004003d, 0x00000006, 0x00000032, 0x00000031, 0x0006000c,
    0x00000006, 0x00000033, 0x00000001, 0x0000000e, 0x00000032, 0x0003003e, 0x00000030, 0x00000033,
    0x00050041, 0x00000026, 0x00000036, 0x0000001e, 0x00000035, 0x0004003d, 0x00000006, 0x00000037,
    0x00000036, 0x0004003d, 0x00000006, 0x00000038, 0x00000030, 0x00050085, 0x00000006, 0x00000039,
    0x00000037, 0x00000038, 0x00050041, 0x00000026, 0x0000003b, 0x0000001e, 0x0000003a, 0x0004003d,
    0x00000006, 0x0000003c, 0x0000003b, 0x0004003d, 0x00000006, 0x0000003d, 0x00000027, 0x00050085,
    0x00000006, 0x0000003e, 0x0000003c, 0x0000003d, 0x00050083, 0x00000006, 0x0000003f, 0x00000039,
    0x0000003e, 0x00050041, 0x00000026, 0x00000040, 0x0000001e, 0x00000035, 0x0004003d, 0x00000006,
    0x00000041, 0x00000040, 0x0004003d, 0x00000006, 0x00000042, 0x00000027, 0x00050085, 0x00000006,
    0x00000043, 0x00000041, 0x00000042, 0x00050041, 0x00000026, 0x00000044, 0x0000001e, 0x0000003a,
    0x0004003d, 0x00000006, 0x00000045, 0x00000044, 0x0004003d, 0x00000006, 0x00000046, 0x00000030,
    0x00050085, 0x00000006, 0x00000047, 0x00000045, 0x00000046, 0x00050081, 0x00000006, 0x00000048,
    0x00000043, 0x00000047, 0x00050050, 0x00000007, 0x00000049, 0x0000003f, 0x00000048, 0x0003003e,
    0x00000034, 0x00000049, 0x00050041, 0x0000002c, 0x0000004b, 0x0000002a, 0x0000004a, 0x0004003d,
    0x00000006, 0x0000004c, 0x0000004b, 0x00050041, 0x00000026, 0x0000004d, 0x00000034, 0x00000035,
    0x0004003d, 0x00000006, 0x0000004e, 0x0000004d, 0x00050088, 0x00000006, 0x0000004f, 0x0000004e,
    0x0000004c, 0x00050041, 0x00000026, 0x00000050, 0x00000034, 0x00000035, 0x0003003e, 0x00000050,
    0x0000004f, 0x0004003d, 0x00000007, 0x00000056, 0x00000034, 0x00050051, 0x00000006, 0x00000057,
    0x00000056, 0x00000000, 0x00050051, 0x00000006, 0x00000058, 0x00000056, 0x00000001, 0x00070050,
    0x00000051, 0x00000059, 0x00000057, 0x00000058, 0x0000000d, 0x00000018, 0x00050041, 0x0000005a,
    0x0000005b, 0x00000055, 0x0000002b, 0x0003003e, 0x0000005b, 0x00000059, 0x0004003d, 0x0000001f,
    0x0000005e, 0x00000021, 0x00050041, 0x0000005f, 0x00000060, 0x00000017, 0x0000005e, 0x0004003d,
    0x00000014, 0x00000061, 0x00000060, 0x0003003e, 0x0000005d, 0x00000061, 0x000100fd, 0x00010038,
};

// Fragment shader: pass-through vertex colors
static const uint32_t fragShaderSpirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000013, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00050005, 0x00000009, 0x4374756f, 0x726f6c6f, 0x00000000, 0x00050005, 0x0000000c,
    0x67617266, 0x6f6c6f43, 0x00000072, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047,
    0x0000000c, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020,
    0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00040017,
    0x0000000a, 0x00000006, 0x00000003, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b,
    0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x00000006, 0x0000000e, 0x3f800000, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a,
    0x0000000d, 0x0000000c, 0x00050051, 0x00000006, 0x0000000f, 0x0000000d, 0x00000000, 0x00050051,
    0x00000006, 0x00000010, 0x0000000d, 0x00000001, 0x00050051, 0x00000006, 0x00000011, 0x0000000d,
    0x00000002, 0x00070050, 0x00000007, 0x00000012, 0x0000000f, 0x00000010, 0x00000011, 0x0000000e,
    0x0003003e, 0x00000009, 0x00000012, 0x000100fd, 0x00010038,
};

// ---------------------------------------------------------------------------
// Globals for signal handler cleanup
// ---------------------------------------------------------------------------
#ifdef _WIN32
static volatile LONG g_running = 1;
#else
static volatile sig_atomic_t g_running = 1;
#endif

static VkInstance        g_instance       = VK_NULL_HANDLE;
static VkDevice          g_device         = VK_NULL_HANDLE;
static VkImage           g_image          = VK_NULL_HANDLE;
static VkDeviceMemory    g_memory         = VK_NULL_HANDLE;
static VkCommandPool     g_commandPool    = VK_NULL_HANDLE;
static VkImageView       g_imageView      = VK_NULL_HANDLE;
static VkRenderPass      g_renderPass     = VK_NULL_HANDLE;
static VkFramebuffer     g_framebuffer    = VK_NULL_HANDLE;
static VkPipelineLayout  g_pipelineLayout = VK_NULL_HANDLE;
static VkPipeline        g_pipeline       = VK_NULL_HANDLE;
static VkShaderModule    g_vertModule     = VK_NULL_HANDLE;
static VkShaderModule    g_fragModule     = VK_NULL_HANDLE;
static SharedMemoryHandle g_memoryHandle  = kInvalidMemoryHandle;
static HandleTransport*  g_transport      = nullptr;

#ifdef _WIN32
static BOOL WINAPI consoleHandler(DWORD) {
    g_running = 0;
    if (g_transport) g_transport->cancel();
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

static VkShaderModule createShaderModule(VkDevice device, const uint32_t* code, size_t sizeBytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.pNext    = nullptr;
    ci.flags    = 0;
    ci.codeSize = sizeBytes;
    ci.pCode    = code;

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &module));
    return module;
}

// ---------------------------------------------------------------------------
// Push constant data (matches vertex shader layout)
// ---------------------------------------------------------------------------
struct PushConstants {
    float angle;
    float aspectRatio;
};

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
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
    // 6. Create VkImageView for the shared image
    // -----------------------------------------------------------------------
    VkImageViewCreateInfo viewCI{};
    viewCI.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.pNext                           = nullptr;
    viewCI.flags                           = 0;
    viewCI.image                           = g_image;
    viewCI.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format                          = imageFormat;
    viewCI.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewCI.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewCI.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewCI.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    VK_CHECK(vkCreateImageView(g_device, &viewCI, nullptr, &g_imageView));
    fprintf(stderr, "[test_renderer] ImageView created\n");

    // -----------------------------------------------------------------------
    // 7. Create render pass
    // -----------------------------------------------------------------------
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = imageFormat;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = nullptr;
    subpass.inputAttachmentCount    = 0;
    subpass.pInputAttachments       = nullptr;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments    = nullptr;
    subpass.pResolveAttachments     = nullptr;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.pNext           = nullptr;
    rpCI.flags           = 0;
    rpCI.attachmentCount = 1;
    rpCI.pAttachments    = &colorAttachment;
    rpCI.subpassCount    = 1;
    rpCI.pSubpasses      = &subpass;
    rpCI.dependencyCount = 0;
    rpCI.pDependencies   = nullptr;

    VK_CHECK(vkCreateRenderPass(g_device, &rpCI, nullptr, &g_renderPass));
    fprintf(stderr, "[test_renderer] RenderPass created\n");

    // -----------------------------------------------------------------------
    // 8. Create framebuffer
    // -----------------------------------------------------------------------
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.pNext           = nullptr;
    fbCI.flags           = 0;
    fbCI.renderPass      = g_renderPass;
    fbCI.attachmentCount = 1;
    fbCI.pAttachments    = &g_imageView;
    fbCI.width           = imageWidth;
    fbCI.height          = imageHeight;
    fbCI.layers          = 1;

    VK_CHECK(vkCreateFramebuffer(g_device, &fbCI, nullptr, &g_framebuffer));
    fprintf(stderr, "[test_renderer] Framebuffer created\n");

    // -----------------------------------------------------------------------
    // 9. Create shader modules
    // -----------------------------------------------------------------------
    g_vertModule = createShaderModule(g_device, vertShaderSpirv, sizeof(vertShaderSpirv));
    g_fragModule = createShaderModule(g_device, fragShaderSpirv, sizeof(fragShaderSpirv));
    fprintf(stderr, "[test_renderer] Shader modules created\n");

    // -----------------------------------------------------------------------
    // 10. Create pipeline layout (push constant: angle + aspectRatio)
    // -----------------------------------------------------------------------
    VkPushConstantRange pushConstRange{};
    pushConstRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstRange.offset     = 0;
    pushConstRange.size       = sizeof(PushConstants); // 8 bytes: 2 floats

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.pNext                  = nullptr;
    plCI.flags                  = 0;
    plCI.setLayoutCount         = 0;
    plCI.pSetLayouts            = nullptr;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pushConstRange;

    VK_CHECK(vkCreatePipelineLayout(g_device, &plCI, nullptr, &g_pipelineLayout));
    fprintf(stderr, "[test_renderer] Pipeline layout created\n");

    // -----------------------------------------------------------------------
    // 11. Create graphics pipeline
    // -----------------------------------------------------------------------
    VkPipelineShaderStageCreateInfo shaderStages[2]{};

    shaderStages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = g_vertModule;
    shaderStages[0].pName  = "main";

    shaderStages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = g_fragModule;
    shaderStages[1].pName  = "main";

    // No vertex input (vertices hardcoded in shader)
    VkPipelineVertexInputStateCreateInfo vertexInputCI{};
    vertexInputCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputCI.vertexBindingDescriptionCount   = 0;
    vertexInputCI.pVertexBindingDescriptions      = nullptr;
    vertexInputCI.vertexAttributeDescriptionCount = 0;
    vertexInputCI.pVertexAttributeDescriptions    = nullptr;

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyCI{};
    inputAssemblyCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyCI.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssemblyCI.primitiveRestartEnable = VK_FALSE;

    // Dynamic viewport and scissor
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(imageWidth);
    viewport.height   = static_cast<float>(imageHeight);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {imageWidth, imageHeight};

    VkPipelineViewportStateCreateInfo viewportCI{};
    viewportCI.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportCI.viewportCount = 1;
    viewportCI.pViewports    = &viewport;
    viewportCI.scissorCount  = 1;
    viewportCI.pScissors     = &scissor;

    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    VkPipelineDynamicStateCreateInfo dynamicStateCI{};
    dynamicStateCI.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateCI.dynamicStateCount = 2;
    dynamicStateCI.pDynamicStates    = dynamicStates;

    VkPipelineRasterizationStateCreateInfo rasterCI{};
    rasterCI.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterCI.depthClampEnable        = VK_FALSE;
    rasterCI.rasterizerDiscardEnable = VK_FALSE;
    rasterCI.polygonMode             = VK_POLYGON_MODE_FILL;
    rasterCI.lineWidth               = 1.0f;
    rasterCI.cullMode                = VK_CULL_MODE_NONE;
    rasterCI.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterCI.depthBiasEnable         = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo msCI{};
    msCI.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msCI.sampleShadingEnable  = VK_FALSE;
    msCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                          VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT |
                                          VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlendCI{};
    colorBlendCI.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendCI.logicOpEnable   = VK_FALSE;
    colorBlendCI.attachmentCount = 1;
    colorBlendCI.pAttachments    = &colorBlendAttachment;

    VkGraphicsPipelineCreateInfo pipelineCI{};
    pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCI.pNext               = nullptr;
    pipelineCI.flags               = 0;
    pipelineCI.stageCount          = 2;
    pipelineCI.pStages             = shaderStages;
    pipelineCI.pVertexInputState   = &vertexInputCI;
    pipelineCI.pInputAssemblyState = &inputAssemblyCI;
    pipelineCI.pTessellationState  = nullptr;
    pipelineCI.pViewportState      = &viewportCI;
    pipelineCI.pRasterizationState = &rasterCI;
    pipelineCI.pMultisampleState   = &msCI;
    pipelineCI.pDepthStencilState  = nullptr;
    pipelineCI.pColorBlendState    = &colorBlendCI;
    pipelineCI.pDynamicState       = &dynamicStateCI;
    pipelineCI.layout              = g_pipelineLayout;
    pipelineCI.renderPass          = g_renderPass;
    pipelineCI.subpass             = 0;
    pipelineCI.basePipelineHandle  = VK_NULL_HANDLE;
    pipelineCI.basePipelineIndex   = -1;

    VK_CHECK(vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pipelineCI,
                                       nullptr, &g_pipeline));
    fprintf(stderr, "[test_renderer] Graphics pipeline created\n");

    // -----------------------------------------------------------------------
    // 12. Listen on socket and accept presenter connection
    // -----------------------------------------------------------------------
    auto transport = HandleTransport::create();
    g_transport = transport.get();
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
    fprintf(stderr, "[test_renderer] Surface shared, handle sent. Starting render loop.\n");

    // -----------------------------------------------------------------------
    // 13. Allocate command buffer and fence for render loop
    // -----------------------------------------------------------------------
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.pNext              = nullptr;
    cmdAllocInfo.commandPool        = g_commandPool;
    cmdAllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(g_device, &cmdAllocInfo, &cmdBuf));

    VkFenceCreateInfo fenceCI{};
    fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCI.pNext = nullptr;
    fenceCI.flags = 0;

    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(g_device, &fenceCI, nullptr, &fence));

    // -----------------------------------------------------------------------
    // 14. Render loop: spinning triangle
    // -----------------------------------------------------------------------
    const float rotationSpeed = 1.0f; // radians per second
    const float aspectRatio = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
    auto startTime = std::chrono::steady_clock::now();

    fprintf(stderr, "[test_renderer] Entering render loop (Ctrl+C to exit)...\n");

    while (g_running) {
        // Calculate angle from elapsed time
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - startTime).count();
        float angle = elapsed * rotationSpeed;

        // Reset command buffer
        VK_CHECK(vkResetCommandBuffer(cmdBuf, 0));

        // Record command buffer
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext            = nullptr;
        beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        VK_CHECK(vkBeginCommandBuffer(cmdBuf, &beginInfo));

        // Begin render pass (clear to dark gray)
        VkClearValue clearValue{};
        clearValue.color = {{ 0.1f, 0.1f, 0.1f, 1.0f }};

        VkRenderPassBeginInfo rpBeginInfo{};
        rpBeginInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBeginInfo.pNext             = nullptr;
        rpBeginInfo.renderPass        = g_renderPass;
        rpBeginInfo.framebuffer       = g_framebuffer;
        rpBeginInfo.renderArea.offset = {0, 0};
        rpBeginInfo.renderArea.extent = {imageWidth, imageHeight};
        rpBeginInfo.clearValueCount   = 1;
        rpBeginInfo.pClearValues      = &clearValue;

        vkCmdBeginRenderPass(cmdBuf, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Set dynamic viewport and scissor
        VkViewport vp{};
        vp.x        = 0.0f;
        vp.y        = 0.0f;
        vp.width    = static_cast<float>(imageWidth);
        vp.height   = static_cast<float>(imageHeight);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmdBuf, 0, 1, &vp);

        VkRect2D sc{};
        sc.offset = {0, 0};
        sc.extent = {imageWidth, imageHeight};
        vkCmdSetScissor(cmdBuf, 0, 1, &sc);

        // Bind pipeline
        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

        // Push constants
        PushConstants pc;
        pc.angle       = angle;
        pc.aspectRatio = aspectRatio;
        vkCmdPushConstants(cmdBuf, g_pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

        // Draw triangle (3 vertices, 1 instance)
        vkCmdDraw(cmdBuf, 3, 1, 0, 0);

        // End render pass (finalLayout handles transition to TRANSFER_SRC_OPTIMAL)
        vkCmdEndRenderPass(cmdBuf);

        VK_CHECK(vkEndCommandBuffer(cmdBuf));

        // Submit with fence
        VK_CHECK(vkResetFences(g_device, 1, &fence));

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

        // Target ~60fps
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    fprintf(stderr, "\n[test_renderer] Shutting down...\n");

    // Wait for GPU to finish before cleanup
    vkDeviceWaitIdle(g_device);

    // -----------------------------------------------------------------------
    // 15. Cleanup
    // -----------------------------------------------------------------------
    transport->close();

    vkDestroyFence(g_device, fence, nullptr);
    vkFreeCommandBuffers(g_device, g_commandPool, 1, &cmdBuf);

    vkDestroyPipeline(g_device, g_pipeline, nullptr);
    vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
    vkDestroyShaderModule(g_device, g_vertModule, nullptr);
    vkDestroyShaderModule(g_device, g_fragModule, nullptr);
    vkDestroyFramebuffer(g_device, g_framebuffer, nullptr);
    vkDestroyRenderPass(g_device, g_renderPass, nullptr);
    vkDestroyImageView(g_device, g_imageView, nullptr);

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
