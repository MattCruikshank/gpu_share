cmake_minimum_required(VERSION 3.19)
project(gpu_share LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- godot-cpp via FetchContent ---
include(FetchContent)
FetchContent_Declare(
    godot-cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    GIT_TAG        godot-4.3-stable
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(godot-cpp)

# --- Vulkan ---
find_package(Vulkan REQUIRED)

# --- Shared library ---
add_library(gpu_share SHARED
    shared_surface.cpp
    frame_capture.cpp
    register_types.cpp
)

target_include_directories(gpu_share PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(gpu_share PRIVATE
    godot-cpp
    Vulkan::Vulkan
)

# Set output name to match .gdextension expectations
set_target_properties(gpu_share PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "gpu_share"
)

# Per-config suffixes matching Godot conventions
set_target_properties(gpu_share PROPERTIES
    DEBUG_POSTFIX ".linux.template_debug.x86_64"
    RELEASE_POSTFIX ".linux.template_release.x86_64"
)
