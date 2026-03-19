#include "vulkan_context.h"
#include "surface_import.h"
#include "compositor.h"
#include "../handle_transport/handle_transport.h"
#include "../shared/shared_handle.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
#ifdef _WIN32
    const char* socketPath = "gpu-share";
#else
    const char* socketPath = "/tmp/gpu-share.sock";
#endif
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--socket") == 0 || strcmp(argv[i], "-s") == 0) && i + 1 < argc) {
            socketPath = argv[++i];
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("GPU Share - Presenter", 1280, 720,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    VulkanContext vkCtx;
    if (!vkCtx.init(window)) {
        fprintf(stderr, "Failed to initialize Vulkan context\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Connect to renderer via transport
    auto transport = HandleTransport::create();
    fprintf(stderr, "Connecting to renderer at %s...\n", socketPath);
    if (!transport->connect(socketPath)) {
        fprintf(stderr, "Failed to connect to renderer socket\n");
        vkCtx.destroy();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    fprintf(stderr, "Connected to renderer\n");

    // Receive shared surface info + memory handle
    SharedMemoryHandle memoryHandle = kInvalidMemoryHandle;
    SharedSurfaceInfo surfaceInfo{};
    if (!transport->recvHandle(memoryHandle, &surfaceInfo, sizeof(surfaceInfo))) {
        fprintf(stderr, "Failed to receive surface info from renderer\n");
        transport->close();
        vkCtx.destroy();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    fprintf(stderr, "Received surface: %ux%u\n",
            surfaceInfo.width, surfaceInfo.height);

    // Import the shared surface
    ImportedSurface imported = importSurface(vkCtx.getDevice(), vkCtx.getPhysicalDevice(),
                                            memoryHandle, surfaceInfo);
    bool hasImportedSurface = (imported.image != VK_NULL_HANDLE);

    Compositor compositor;

    // Main loop
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN &&
                event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        uint32_t imageIndex = vkCtx.acquireNextImage();
        VkCommandBuffer cmd = vkCtx.getCommandBuffer(imageIndex);

        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

        VkImage swapImage = vkCtx.getSwapchainImage(imageIndex);
        VkExtent2D extent = vkCtx.getSwapchainExtent();

        if (hasImportedSurface) {
            compositor.recordBlit(cmd,
                imported.image, imported.info.width, imported.info.height,
                swapImage, extent.width, extent.height);
        } else {
            compositor.recordClear(cmd, swapImage, extent.width, extent.height);
        }

        VK_CHECK(vkEndCommandBuffer(cmd));

        vkCtx.submitAndPresent(imageIndex, cmd);
    }

    vkDeviceWaitIdle(vkCtx.getDevice());

    imported.destroy(vkCtx.getDevice());
    vkCtx.destroy();
    transport->close();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
