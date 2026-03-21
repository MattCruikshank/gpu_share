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

    # deno_renderer's build.rs needs protoc for tonic codegen.
    # GPU_SHARE_PROTOC is set by proto/CMakeLists.txt — it may be a
    # generator expression ($<TARGET_FILE:protoc>) when using FetchContent,
    # or a plain path when using system packages.
    # For cargo's PROTOC env var we need a real path at build time.
    find_program(PROTOC_EXE protoc)
    if(NOT PROTOC_EXE AND TARGET protoc)
        # FetchContent case: protoc is a build target, use its output path
        set(PROTOC_EXE $<TARGET_FILE:protoc>)
    endif()

    # Regenerate TypeScript protobuf types + bundle when the proto file changes.
    # Runs protoc-gen-es and esbuild directly (avoids npm needing protoc on PATH).
    find_program(PROTOC_GEN_ES protoc-gen-es
        PATHS "${CMAKE_SOURCE_DIR}/deno_renderer/node_modules/.bin"
        NO_DEFAULT_PATH)
    find_program(ESBUILD esbuild
        PATHS "${CMAKE_SOURCE_DIR}/deno_renderer/node_modules/.bin"
        NO_DEFAULT_PATH)

    if(PROTOC_EXE AND PROTOC_GEN_ES AND ESBUILD)
        set(PROTO_BUNDLE "${CMAKE_SOURCE_DIR}/deno_renderer/proto_bundle.js")
        set(DENO_GEN_DIR "${CMAKE_SOURCE_DIR}/deno_renderer/gen")
        add_custom_command(
            OUTPUT "${PROTO_BUNDLE}"
            # Step 1: generate TypeScript types
            COMMAND ${PROTOC_EXE}
                --proto_path="${CMAKE_SOURCE_DIR}/proto"
                "--es_out=${DENO_GEN_DIR}"
                "--plugin=protoc-gen-es=${PROTOC_GEN_ES}"
                "${CMAKE_SOURCE_DIR}/proto/gpu_share.proto"
            # Step 2: bundle into single IIFE
            COMMAND ${ESBUILD}
                "${CMAKE_SOURCE_DIR}/deno_renderer/proto_entry.js"
                --bundle --format=iife --outfile=${PROTO_BUNDLE}
                --platform=neutral --target=es2020
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/deno_renderer"
            DEPENDS "${CMAKE_SOURCE_DIR}/proto/gpu_share.proto"
            COMMENT "Regenerating TypeScript protobuf types + bundle"
        )
        add_custom_target(deno_proto_bundle DEPENDS "${PROTO_BUNDLE}")
    else()
        message(STATUS "protoc/protoc-gen-es/esbuild not all found — run 'cd deno_renderer && npm install && npm run proto' manually")
    endif()

    add_custom_target(deno_renderer ALL
        COMMAND ${CMAKE_COMMAND} -E env "PROTOC=${PROTOC_EXE}"
            ${CARGO_EXE} build ${CARGO_BUILD_FLAG}
            --manifest-path "${CMAKE_SOURCE_DIR}/deno_renderer/Cargo.toml"
        COMMENT "Building deno_renderer (Rust/cargo)"
        BYPRODUCTS ${DENO_RENDERER_EXE}
    )
    if(TARGET deno_proto_bundle)
        add_dependencies(deno_renderer deno_proto_bundle)
    endif()
    # If protoc comes from FetchContent, cargo must wait for it to be built
    if(TARGET protoc)
        add_dependencies(deno_renderer protoc)
    endif()

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
