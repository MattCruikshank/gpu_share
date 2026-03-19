cmake_minimum_required(VERSION 3.25)
project(gpu-share LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(presenter)
add_subdirectory(test_renderer)

option(BUILD_RENDERER_EXTENSION "Build Godot renderer extension (requires godot-cpp)" OFF)
if(BUILD_RENDERER_EXTENSION)
    add_subdirectory(renderer_extension)
endif()
