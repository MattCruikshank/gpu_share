find_package(Vulkan REQUIRED)

find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
    )
    set(SDL_VULKAN ON CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_WAYLAND ON CACHE BOOL "" FORCE)
    set(SDL_X11 ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
endif()

if(WIN32)
    set(TRANSPORT_SOURCES ../handle_transport/handle_transport_win32.cpp)
else()
    set(TRANSPORT_SOURCES ../handle_transport/handle_transport_unix.cpp)
endif()

add_executable(presenter
    main.cpp
    vulkan_context.cpp
    surface_import.cpp
    compositor.cpp
    ${TRANSPORT_SOURCES}
)

target_include_directories(presenter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(TARGET SDL3::SDL3-static)
    target_link_libraries(presenter PRIVATE Vulkan::Vulkan SDL3::SDL3-static)
else()
    target_link_libraries(presenter PRIVATE Vulkan::Vulkan SDL3::SDL3)
endif()

target_compile_features(presenter PRIVATE cxx_std_17)

if(WIN32)
    target_compile_definitions(presenter PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
