#pragma once
#include <vulkan/vulkan.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  using SharedMemoryHandle = HANDLE;
  using SharedSemaphoreHandle = HANDLE;
  constexpr auto kExternalMemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  constexpr auto kExternalSemaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  constexpr SharedMemoryHandle kInvalidMemoryHandle = nullptr;
  constexpr SharedSemaphoreHandle kInvalidSemaphoreHandle = nullptr;
#else
  using SharedMemoryHandle = int;
  using SharedSemaphoreHandle = int;
  constexpr auto kExternalMemoryHandleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
  constexpr auto kExternalSemaphoreHandleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
  constexpr SharedMemoryHandle kInvalidMemoryHandle = -1;
  constexpr SharedSemaphoreHandle kInvalidSemaphoreHandle = -1;
#endif

struct SharedSurfaceInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkDeviceSize memorySize = 0;
    uint32_t memoryTypeBits = 0;
};
