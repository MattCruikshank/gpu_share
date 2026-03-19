find_package(Vulkan REQUIRED)

if(WIN32)
    set(TRANSPORT_SOURCES ../handle_transport/handle_transport_win32.cpp)
else()
    set(TRANSPORT_SOURCES ../handle_transport/handle_transport_unix.cpp)
endif()

add_executable(test_renderer
    main.cpp
    ${TRANSPORT_SOURCES}
)

target_include_directories(test_renderer PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_renderer PRIVATE Vulkan::Vulkan)
target_compile_features(test_renderer PRIVATE cxx_std_17)

if(WIN32)
    target_compile_definitions(test_renderer PRIVATE VK_USE_PLATFORM_WIN32_KHR)
endif()
