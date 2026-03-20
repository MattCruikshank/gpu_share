#include "vulkan_context.h"
#include "surface_import.h"
#include "compositor.h"
#include "../handle_transport/handle_transport.h"
#include "../shared/shared_handle.h"
#include "../shared/input_event.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

static void forwardEvent(HandleTransport* t, const InputEvent& ev) {
    t->sendData(&ev, sizeof(ev));
}

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
    bool mouseDragging = false;
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
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                vkCtx.notifyResized();
                // Tell renderer to resize its shared surface
                if (hasImportedSurface) {
                    int pw, ph;
                    SDL_GetWindowSizeInPixels(window, &pw, &ph);
                    InputEvent re{};
                    re.type = InputEventType::Resize;
                    re.resize.width = static_cast<uint32_t>(pw);
                    re.resize.height = static_cast<uint32_t>(ph);
                    forwardEvent(transport.get(), re);
                }
            }

            // Forward input to renderer
            InputEvent ie{};
            bool send = false;

            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                ie.type = InputEventType::MouseButton;
                ie.mouseButton.button = event.button.button;
                ie.mouseButton.pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? 1 : 0;
                ie.mouseButton.x = event.button.x;
                ie.mouseButton.y = event.button.y;
                send = true;
                mouseDragging = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                                 event.button.button == 1);
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION && mouseDragging) {
                ie.type = InputEventType::MouseMotion;
                ie.motion.x = event.motion.x;
                ie.motion.y = event.motion.y;
                ie.motion.relX = event.motion.xrel;
                ie.motion.relY = event.motion.yrel;
                send = true;
            }
            else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                ie.type = InputEventType::MouseWheel;
                ie.wheel.scrollY = event.wheel.y;
                send = true;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                ie.type = (event.type == SDL_EVENT_KEY_DOWN)
                          ? InputEventType::KeyDown : InputEventType::KeyUp;
                ie.key.scancode = event.key.scancode;
                send = true;
            }

            if (send && hasImportedSurface) {
                forwardEvent(transport.get(), ie);
            }
        }

        // Check if renderer sent a new shared surface (after resize)
        SharedMemoryHandle newHandle = kInvalidMemoryHandle;
        SharedSurfaceInfo newInfo{};
        if (transport->recvHandleNonBlocking(newHandle, &newInfo, sizeof(newInfo))) {
            vkDeviceWaitIdle(vkCtx.getDevice());
            imported.destroy(vkCtx.getDevice());
            imported = importSurface(vkCtx.getDevice(), vkCtx.getPhysicalDevice(),
                                     newHandle, newInfo);
            hasImportedSurface = (imported.image != VK_NULL_HANDLE);
            fprintf(stderr, "Re-imported surface: %ux%u\n", newInfo.width, newInfo.height);
        }

        uint32_t imageIndex = 0;
        if (!vkCtx.acquireNextImage(imageIndex)) continue;
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
