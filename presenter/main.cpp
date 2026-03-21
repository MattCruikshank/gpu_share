#include "vulkan_context.h"
#include "surface_import.h"
#include "compositor.h"
#include "grpc_server.h"
#include "../shared/shared_handle.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #include <signal.h>
#endif

// ---------------------------------------------------------------------------
// Child process management
// ---------------------------------------------------------------------------
struct ChildProcess {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    bool valid = false;

    bool spawn(const std::string& exe, const std::string& args) {
        std::string cmdLine = "\"" + exe + "\" " + args;
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        if (!CreateProcessA(nullptr, const_cast<char*>(cmdLine.c_str()),
                            nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &si, &pi)) {
            fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
            return false;
        }
        valid = true;
        return true;
    }

    void terminate() {
        if (!valid) return;
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        valid = false;
    }
#else
    pid_t pid = -1;

    bool spawn(const std::string& exe, const std::string& args) {
        pid = fork();
        if (pid < 0) {
            perror("fork");
            return false;
        }
        if (pid == 0) {
            // Child — parse args and exec
            std::string portArg;
            auto pos = args.find("--port ");
            if (pos != std::string::npos) {
                portArg = args.substr(pos + 7);
                while (!portArg.empty() && portArg.back() == ' ')
                    portArg.pop_back();
            }
            if (!portArg.empty()) {
                execlp(exe.c_str(), exe.c_str(), "--port", portArg.c_str(), nullptr);
            } else {
                execlp(exe.c_str(), exe.c_str(), nullptr);
            }
            perror("execlp");
            _exit(1);
        }
        return true;
    }

    void terminate() {
        if (pid <= 0) return;
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
        pid = -1;
    }
#endif
};

// ---------------------------------------------------------------------------
// Find the renderer executable next to the presenter
// ---------------------------------------------------------------------------
static bool fileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static std::string findExe(const char* presenterArgv0, const std::string& name) {
    std::string path(presenterArgv0);
    auto lastSep = path.find_last_of("/\\");

#ifdef _WIN32
    std::string ext = ".exe";
#else
    std::string ext;
#endif

    // Same directory as presenter
    std::string sameDir;
    if (lastSep != std::string::npos)
        sameDir = path.substr(0, lastSep + 1);
    std::string candidate = sameDir + name + ext;
    if (fileExists(candidate)) return candidate;

    // MSVC multi-config: build/presenter/Debug/ → build/<name>/Debug/
    if (lastSep != std::string::npos) {
        auto configSep = path.find_last_of("/\\", lastSep - 1);
        if (configSep != std::string::npos) {
            std::string configDir = path.substr(configSep, lastSep - configSep + 1);
            auto parentSep = path.find_last_of("/\\", configSep - 1);
            if (parentSep != std::string::npos) {
                std::string buildRoot = path.substr(0, parentSep + 1);
                candidate = buildRoot + name + configDir + name + ext;
                if (fileExists(candidate)) return candidate;
            }
        }
    }

    // Cargo build: walk up from exe dir to find <root>/<name>/target/debug/<name>
    if (lastSep != std::string::npos) {
        std::string sep(1, path[lastSep]);
        std::string dir = path.substr(0, lastSep);
        for (int i = 0; i < 5; i++) {
            candidate = dir + sep + name + sep + "target" + sep + "debug" + sep + name + ext;
            if (fileExists(candidate)) return candidate;
            candidate = dir + sep + name + sep + "target" + sep + "release" + sep + name + ext;
            if (fileExists(candidate)) return candidate;
            auto up = dir.find_last_of("/\\");
            if (up == std::string::npos) break;
            dir = dir.substr(0, up);
        }
    }

    // Try from current working directory
#ifdef _WIN32
    std::string sep = "\\";
#else
    std::string sep = "/";
#endif
    candidate = name + sep + "target" + sep + "debug" + sep + name + ext;
    if (fileExists(candidate)) return candidate;
    candidate = name + sep + "target" + sep + "release" + sep + name + ext;
    if (fileExists(candidate)) return candidate;

    // Fallback
    return name + ext;
}

int main(int argc, char* argv[]) {
    const char* port = "9710";
    const char* rendererPath = nullptr;
    bool noSpawn = false;
    bool useDeno = false;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
            rendererPath = argv[++i];
        } else if (strcmp(argv[i], "--no-spawn") == 0) {
            noSpawn = true;
        } else if (strcmp(argv[i], "--deno") == 0) {
            useDeno = true;
        }
    }

    // Determine renderer exe path
    std::string rendererExe;
    if (rendererPath) {
        rendererExe = rendererPath;
    } else {
        std::string name = useDeno ? "deno_renderer" : "test_renderer";
        rendererExe = findExe(argv[0], name);
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

    // Start gRPC server before spawning renderer
    GrpcBridge bridge;
    uint16_t portNum = static_cast<uint16_t>(std::stoi(port));
    if (!bridge.start(portNum)) {
        fprintf(stderr, "Failed to start gRPC server\n");
        vkCtx.destroy();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Launch renderer process (unless --no-spawn)
    ChildProcess renderer;
    if (!noSpawn) {
        std::string rendererArgs = std::string("--port ") + port;
        fprintf(stderr, "Launching renderer: %s %s\n",
                rendererExe.c_str(), rendererArgs.c_str());
        if (!renderer.spawn(rendererExe, rendererArgs)) {
            fprintf(stderr, "Failed to launch renderer\n");
            bridge.stop();
            vkCtx.destroy();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // Wait for renderer to connect via gRPC
    SharedMemoryHandle memoryHandle = kInvalidMemoryHandle;
    SharedSurfaceInfo surfaceInfo{};
    fprintf(stderr, "Waiting for renderer to connect via gRPC...\n");
    if (!bridge.waitForRenderer(memoryHandle, surfaceInfo)) {
        fprintf(stderr, "Failed to receive surface info from renderer\n");
        bridge.stop();
        renderer.terminate();
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
                    bridge.pushResize(static_cast<uint32_t>(pw),
                                      static_cast<uint32_t>(ph));
                }
            }

            // Forward input to renderer
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                bridge.pushMouseButton(event.button.button, pressed,
                                       event.button.x, event.button.y);
                mouseDragging = (pressed && event.button.button == 1);
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION && mouseDragging) {
                bridge.pushMouseMotion(event.motion.x, event.motion.y,
                                       event.motion.xrel, event.motion.yrel);
            }
            else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                bridge.pushMouseWheel(event.wheel.y);
            }
            else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    bridge.pushKeyDown(event.key.scancode);
                } else {
                    bridge.pushKeyUp(event.key.scancode);
                }
            }
        }

        // Check if renderer sent a new shared surface (after resize)
        SharedMemoryHandle newHandle = kInvalidMemoryHandle;
        SharedSurfaceInfo newInfo{};
        if (bridge.pollSurfaceUpdate(newHandle, newInfo)) {
            vkDeviceWaitIdle(vkCtx.getDevice());
            imported.destroy(vkCtx.getDevice());
            imported = importSurface(vkCtx.getDevice(), vkCtx.getPhysicalDevice(),
                                     newHandle, newInfo);
            hasImportedSurface = (imported.image != VK_NULL_HANDLE);
            fprintf(stderr, "Re-imported surface: %ux%u\n", newInfo.width, newInfo.height);
        }

        uint32_t imageIndex = 0;
        if (!vkCtx.acquireNextImage(imageIndex)) continue;
        VkCommandBuffer cmd = vkCtx.getCurrentCommandBuffer();

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
    bridge.stop();
    renderer.terminate();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
