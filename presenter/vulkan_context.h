#pragma once

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vector>
#include <cstdio>
#include <cstdlib>

#define VK_CHECK(call)                                                         \
    do {                                                                        \
        VkResult result_ = (call);                                              \
        if (result_ != VK_SUCCESS) {                                            \
            fprintf(stderr, "Vulkan error %d at %s:%d\n",                       \
                    static_cast<int>(result_), __FILE__, __LINE__);             \
            std::abort();                                                       \
        }                                                                       \
    } while (0)

class VulkanContext {
public:
    bool init(SDL_Window* window);
    // Returns false if swapchain was recreated (caller should skip this frame).
    bool acquireNextImage(uint32_t& imageIndex);
    void submitAndPresent(uint32_t imageIndex, VkCommandBuffer cmdBuf);
    void destroy();

    void notifyResized() { framebufferResized_ = true; }

    VkDevice getDevice() const { return device_; }
    VkPhysicalDevice getPhysicalDevice() const { return physicalDevice_; }
    VkCommandPool getCommandPool() const { return commandPool_; }
    VkQueue getQueue() const { return queue_; }
    uint32_t getQueueFamily() const { return queueFamily_; }
    VkFormat getSwapchainFormat() const { return swapchainFormat_; }
    VkExtent2D getSwapchainExtent() const { return swapchainExtent_; }
    VkImage getSwapchainImage(uint32_t index) const { return swapchainImages_[index]; }
    VkCommandBuffer getCommandBuffer(uint32_t index) const { return commandBuffers_[index]; }
    VkCommandBuffer getCurrentCommandBuffer() const { return commandBuffers_[currentFrame_]; }
    uint32_t swapchainImageCount() const { return static_cast<uint32_t>(swapchainImages_.size()); }

    // Imported function pointers
#ifdef _WIN32
    PFN_vkImportSemaphoreWin32HandleKHR fpImportSemaphoreWin32HandleKHR = nullptr;
#else
    PFN_vkImportSemaphoreFdKHR fpImportSemaphoreFdKHR = nullptr;
#endif

private:
    bool createInstance();
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSwapchain();
    void cleanupSwapchain();
    void recreateSwapchain();
    bool createCommandPool();
    bool createSyncObjects();
    void loadFunctionPointers();

    SDL_Window* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_ = {};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    // Semaphores indexed by swapchain image index (not frame counter).
    // This avoids the semaphore reuse hazard where the presentation engine
    // still holds a semaphore from a previous present of a different image.
    std::vector<VkSemaphore> imageAvailableSemaphores_; // per swapchain image
    std::vector<VkSemaphore> renderFinishedSemaphores_; // per swapchain image
    // Fences indexed by frame-in-flight counter to limit concurrent frames.
    std::vector<VkFence> inFlightFences_;
    // Track which fence is associated with each swapchain image
    std::vector<VkFence> imagesInFlight_;
    uint32_t currentFrame_ = 0;
    uint32_t maxFramesInFlight_ = 2;
    bool framebufferResized_ = false;

    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
};
