cmake_minimum_required(VERSION 3.25)
project(gpu-share LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(proto)
add_subdirectory(presenter)
add_subdirectory(test_renderer)

option(BUILD_RENDERER_EXTENSION "Build Godot renderer extension (requires godot-cpp)" OFF)
if(BUILD_RENDERER_EXTENSION)
    add_subdirectory(renderer_extension)
endif()

# Build deno_renderer via cargo if Rust is available
find_program(CARGO_EXE cargo)
if(CARGO_EXE)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(CARGO_BUILD_FLAG "--release")
        set(CARGO_OUTPUT_DIR "release")
    else()
        set(CARGO_BUILD_FLAG "")
        set(CARGO_OUTPUT_DIR "debug")
    endif()

    if(WIN32)
        set(DENO_RENDERER_EXE "${CMAKE_SOURCE_DIR}/deno_renderer/target/${CARGO_OUTPUT_DIR}/deno_renderer.exe")
    else()
        set(DENO_RENDERER_EXE "${CMAKE_SOURCE_DIR}/deno_renderer/target/${CARGO_OUTPUT_DIR}/deno_renderer")
    endif()

    # deno_renderer's build.rs needs protoc for tonic codegen
    find_program(PROTOC_EXE protoc)

    add_custom_target(deno_renderer ALL
        COMMAND ${CMAKE_COMMAND} -E env "PROTOC=${PROTOC_EXE}"
            ${CARGO_EXE} build ${CARGO_BUILD_FLAG}
            --manifest-path "${CMAKE_SOURCE_DIR}/deno_renderer/Cargo.toml"
        COMMENT "Building deno_renderer (Rust/cargo)"
        BYPRODUCTS ${DENO_RENDERER_EXE}
    )

    # Copy scene.ts and proto_bundle.js next to the binary so it can find them
    add_custom_command(TARGET deno_renderer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/deno_renderer/scene.ts"
            "${CMAKE_SOURCE_DIR}/deno_renderer/target/${CARGO_OUTPUT_DIR}/scene.ts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/deno_renderer/proto_bundle.js"
            "${CMAKE_SOURCE_DIR}/deno_renderer/target/${CARGO_OUTPUT_DIR}/proto_bundle.js"
        COMMENT "Copying scene.ts and proto_bundle.js to cargo output"
    )
else()
    message(STATUS "cargo not found — skipping deno_renderer (install Rust from https://rustup.rs)")
endif()
