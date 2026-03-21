find_package(Vulkan REQUIRED)

add_executable(test_renderer
    main.cpp
)

target_include_directories(test_renderer PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_renderer PRIVATE Vulkan::Vulkan gpu_share_proto)
target_compile_features(test_renderer PRIVATE cxx_std_17)

if(WIN32)
    target_compile_definitions(test_renderer PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    target_link_libraries(test_renderer PRIVATE ws2_32)
endif()
