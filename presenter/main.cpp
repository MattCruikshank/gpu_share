#include "vulkan_context.h"
#include "surface_import.h"
#include "compositor.h"
#include "grpc_server.h"
#include "../shared/shared_handle.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#include <thread>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #include <signal.h>
#endif

static constexpr int MAX_TABS = 9;
static constexpr uint16_t BASE_PORT = 9710;

// ---------------------------------------------------------------------------
// Child process management
// ---------------------------------------------------------------------------
struct ChildProcess {
#ifdef _WIN32
    PROCESS_INFORMATION pi{};
    bool valid = false;

    bool spawn(const std::string& exe, const std::vector<std::string>& args) {
        std::string cmdLine = "\"" + exe + "\"";
        for (auto& a : args) cmdLine += " " + a;
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

    bool spawn(const std::string& exe, const std::vector<std::string>& args) {
        pid = fork();
        if (pid < 0) { perror("fork"); return false; }
        if (pid == 0) {
            // Build argv for execv
            std::vector<const char*> argv;
            argv.push_back(exe.c_str());
            for (auto& a : args) argv.push_back(a.c_str());
            argv.push_back(nullptr);
            execv(exe.c_str(), const_cast<char* const*>(argv.data()));
            perror("execv");
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
// Tab: one renderer process + its gRPC bridge + imported surface
// ---------------------------------------------------------------------------
struct Tab {
    int index = -1;                          // 0-based (tab "1" = index 0)
    std::string scriptName;                  // e.g. "scenes/1.ts"
    uint16_t port = 0;
    std::unique_ptr<GrpcBridge> bridge;
    ChildProcess process;
    ImportedSurface imported{};
    bool spawned = false;
    bool connected = false;
    bool hasImportedSurface = false;
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

    std::string sameDir;
    if (lastSep != std::string::npos)
        sameDir = path.substr(0, lastSep + 1);
    std::string candidate = sameDir + name + ext;
    if (fileExists(candidate)) return candidate;

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

#ifdef _WIN32
    std::string sep = "\\";
#else
    std::string sep = "/";
#endif
    candidate = name + sep + "target" + sep + "debug" + sep + name + ext;
    if (fileExists(candidate)) return candidate;
    candidate = name + sep + "target" + sep + "release" + sep + name + ext;
    if (fileExists(candidate)) return candidate;

    return name + ext;
}

// ---------------------------------------------------------------------------
// Tear down a tab's renderer (for hot-reload or cleanup)
// ---------------------------------------------------------------------------
static void teardownTab(Tab& tab, VkDevice device) {
    if (tab.hasImportedSurface) {
        vkDeviceWaitIdle(device);
        tab.imported.destroy(device);
        tab.hasImportedSurface = false;
    }
    if (tab.bridge) {
        tab.bridge->stop();
        tab.bridge.reset();
    }
    if (tab.spawned) {
        tab.process.terminate();
    }
    tab.spawned = false;
    tab.connected = false;
}

// ---------------------------------------------------------------------------
// Resolve a scene script path. Checks multiple locations:
//   1. deno_renderer/<scriptName> (source tree, if running from repo root)
//   2. <renderer exe dir>/<scriptName> (next to the binary)
//   3. <scriptName> as-is (relative to CWD)
// ---------------------------------------------------------------------------
static std::string resolveScriptPath(const std::string& rendererExe,
                                     const std::string& scriptName) {
    // Source tree (e.g. deno_renderer/scenes/1.ts from repo root)
    std::string candidate = "deno_renderer/" + scriptName;
    if (fileExists(candidate)) return candidate;
#ifdef _WIN32
    candidate = "deno_renderer\\" + scriptName;
    if (fileExists(candidate)) return candidate;
#endif

    // Next to renderer executable
    auto lastSep = rendererExe.find_last_of("/\\");
    if (lastSep != std::string::npos) {
        candidate = rendererExe.substr(0, lastSep + 1) + scriptName;
        if (fileExists(candidate)) return candidate;
    }

    // Fallback: as-is
    return scriptName;
}

// ---------------------------------------------------------------------------
// Spawn and connect a tab's renderer
// ---------------------------------------------------------------------------
static bool spawnTab(Tab& tab, const std::string& rendererExe, VkDevice device,
                     VkPhysicalDevice physDevice) {
    // Start gRPC server for this tab
    tab.bridge = std::make_unique<GrpcBridge>();
    if (!tab.bridge->start(tab.port)) {
        fprintf(stderr, "[tab %d] Failed to start gRPC on port %u\n", tab.index + 1, tab.port);
        return false;
    }

    // URLs pass through directly; local paths get resolved
    std::string scriptPath;
    if (tab.scriptName.find("://") != std::string::npos) {
        scriptPath = tab.scriptName;  // URL — pass as-is
    } else {
        scriptPath = resolveScriptPath(rendererExe, tab.scriptName);
    }

    // Spawn renderer
    std::vector<std::string> args = {
        "--port", std::to_string(tab.port),
        "--script", scriptPath
    };
    fprintf(stderr, "[tab %d] Launching: %s --port %u --script %s\n",
            tab.index + 1, rendererExe.c_str(), tab.port, tab.scriptName.c_str());
    if (!tab.process.spawn(rendererExe, args)) {
        fprintf(stderr, "[tab %d] Failed to spawn renderer\n", tab.index + 1);
        tab.bridge->stop();
        tab.bridge.reset();
        return false;
    }

    tab.spawned = true;

    // Wait for renderer to connect (in a background thread so we don't block the main loop)
    // For simplicity, do a short blocking wait here
    SharedMemoryHandle handle = kInvalidMemoryHandle;
    SharedSurfaceInfo info{};
    if (!tab.bridge->waitForRenderer(handle, info, 15000)) {
        fprintf(stderr, "[tab %d] Renderer failed to connect\n", tab.index + 1);
        return false;
    }

    tab.imported = importSurface(device, physDevice, handle, info);
    tab.hasImportedSurface = (tab.imported.image != VK_NULL_HANDLE);
    tab.connected = true;

    fprintf(stderr, "[tab %d] Connected: %ux%u\n", tab.index + 1, info.width, info.height);
    return true;
}

// ---------------------------------------------------------------------------
// Load scenes.json → map of tab number ("1".."9") to script URL/path
// ---------------------------------------------------------------------------
static std::map<int, std::string> loadScenesJson(const std::string& path) {
    std::map<int, std::string> scenes;
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Warning: could not open %s\n", path.c_str());
        return scenes;
    }
    // Minimal JSON parsing for { "1": "...", "2": "..." } format
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    // Find each "N": "value" pair
    size_t pos = 0;
    while (pos < content.size()) {
        // Find key
        auto kStart = content.find('"', pos);
        if (kStart == std::string::npos) break;
        auto kEnd = content.find('"', kStart + 1);
        if (kEnd == std::string::npos) break;
        std::string key = content.substr(kStart + 1, kEnd - kStart - 1);

        // Find value
        auto vStart = content.find('"', kEnd + 1);
        if (vStart == std::string::npos) break;
        auto vEnd = content.find('"', vStart + 1);
        if (vEnd == std::string::npos) break;
        std::string value = content.substr(vStart + 1, vEnd - vStart - 1);

        int tabNum = std::atoi(key.c_str());
        if (tabNum >= 1 && tabNum <= 9) {
            scenes[tabNum - 1] = value;  // 0-based
        }
        pos = vEnd + 1;
    }
    return scenes;
}

int main(int argc, char* argv[]) {
    uint16_t basePort = BASE_PORT;
    std::string scenesJsonPath = "scenes.json";

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            basePort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (strcmp(argv[i], "--scenes") == 0 && i + 1 < argc) {
            scenesJsonPath = argv[++i];
        }
    }

    std::string rendererExe = findExe(argv[0], "deno_renderer");

    // Load scene mappings from scenes.json
    auto sceneMap = loadScenesJson(scenesJsonPath);
    if (!sceneMap.empty()) {
        fprintf(stderr, "Loaded %zu scene(s) from %s\n",
                sceneMap.size(), scenesJsonPath.c_str());
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

    // Tab state — use scenes.json URLs if available, fall back to local paths
    Tab tabs[MAX_TABS];
    for (int i = 0; i < MAX_TABS; i++) {
        tabs[i].index = i;
        auto it = sceneMap.find(i);
        if (it != sceneMap.end()) {
            tabs[i].scriptName = it->second;
        } else {
            tabs[i].scriptName = "scenes/" + std::to_string(i + 1) + ".ts";
        }
        tabs[i].port = basePort + static_cast<uint16_t>(i);
    }
    int activeTab = -1;  // -1 = no active tab

    Compositor compositor;

    fprintf(stderr, "Presenter ready. 1-%d=tabs, R=reload, ESC=quit\n", MAX_TABS);

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

            // Number keys 1-9: switch/launch tabs
            if (event.type == SDL_EVENT_KEY_DOWN) {
                int tabNum = -1;
                SDL_Keycode key = event.key.key;
                if (key >= SDLK_1 && key <= SDLK_9) {
                    tabNum = key - SDLK_1;  // 0-based
                }
                if (tabNum >= 0 && tabNum < MAX_TABS && tabNum != activeTab) {
                    // Pause the old active tab
                    if (activeTab >= 0 && tabs[activeTab].connected) {
                        tabs[activeTab].bridge->pushTabPause();
                    }

                    // Lazy-spawn if needed
                    if (!tabs[tabNum].spawned) {
                        fprintf(stderr, "Spawning tab %d...\n", tabNum + 1);
                        if (!spawnTab(tabs[tabNum], rendererExe,
                                      vkCtx.getDevice(), vkCtx.getPhysicalDevice())) {
                            fprintf(stderr, "Failed to spawn tab %d\n", tabNum + 1);
                            continue;
                        }
                    }

                    // Resume the new tab
                    if (tabs[tabNum].connected) {
                        tabs[tabNum].bridge->pushTabResume();
                    }

                    activeTab = tabNum;
                    fprintf(stderr, "Switched to tab %d\n", activeTab + 1);

                    // Update window title
                    std::string title = "GPU Share - Tab " + std::to_string(activeTab + 1);
                    SDL_SetWindowTitle(window, title.c_str());
                }

                // R key: hot-reload active tab
                if (key == SDLK_R && activeTab >= 0 && tabs[activeTab].spawned) {
                    fprintf(stderr, "[tab %d] Hot-reloading...\n", activeTab + 1);
                    teardownTab(tabs[activeTab], vkCtx.getDevice());
                    if (spawnTab(tabs[activeTab], rendererExe,
                                 vkCtx.getDevice(), vkCtx.getPhysicalDevice())) {
                        tabs[activeTab].bridge->pushTabResume();
                        fprintf(stderr, "[tab %d] Reloaded\n", activeTab + 1);
                    } else {
                        fprintf(stderr, "[tab %d] Reload failed\n", activeTab + 1);
                        activeTab = -1;
                        SDL_SetWindowTitle(window, "GPU Share - Presenter");
                    }
                }
            }

            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                vkCtx.notifyResized();
                // Tell ALL connected renderers to resize (not just active)
                int pw, ph;
                SDL_GetWindowSizeInPixels(window, &pw, &ph);
                for (int i = 0; i < MAX_TABS; i++) {
                    if (tabs[i].connected && tabs[i].hasImportedSurface) {
                        tabs[i].bridge->pushResize(
                            static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
                    }
                }
            }

            // Forward input to active renderer only
            if (activeTab >= 0 && tabs[activeTab].connected) {
                auto& bridge = *tabs[activeTab].bridge;

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
        }

        // Poll all connected tabs for surface updates (resize responses)
        for (int i = 0; i < MAX_TABS; i++) {
            if (!tabs[i].connected) continue;
            SharedMemoryHandle newHandle = kInvalidMemoryHandle;
            SharedSurfaceInfo newInfo{};
            if (tabs[i].bridge->pollSurfaceUpdate(newHandle, newInfo)) {
                vkDeviceWaitIdle(vkCtx.getDevice());
                tabs[i].imported.destroy(vkCtx.getDevice());
                tabs[i].imported = importSurface(vkCtx.getDevice(), vkCtx.getPhysicalDevice(),
                                                  newHandle, newInfo);
                tabs[i].hasImportedSurface = (tabs[i].imported.image != VK_NULL_HANDLE);
                fprintf(stderr, "[tab %d] Re-imported surface: %ux%u\n",
                        i + 1, newInfo.width, newInfo.height);
            }
        }

        // Render
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

        if (activeTab >= 0 && tabs[activeTab].hasImportedSurface) {
            auto& imp = tabs[activeTab].imported;
            compositor.recordBlit(cmd,
                imp.image, imp.info.width, imp.info.height,
                swapImage, extent.width, extent.height);
        } else {
            compositor.recordClear(cmd, swapImage, extent.width, extent.height);
        }

        VK_CHECK(vkEndCommandBuffer(cmd));

        vkCtx.submitAndPresent(imageIndex, cmd);
    }

    // Cleanup
    for (int i = 0; i < MAX_TABS; i++) {
        teardownTab(tabs[i], vkCtx.getDevice());
    }

    vkCtx.destroy();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
