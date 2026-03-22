# Generate C++ code from gpu_share.proto (protobuf + gRPC)
#
# Discovery order:
#   1. CMake CONFIG packages (vcpkg, system CMake configs)
#   2. pkg-config (Linux system packages)
#   3. FetchContent (download and build from source)

set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/gpu_share.proto")
set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}")

# --- Find gRPC + protobuf ---

# Try CMake CONFIG mode first (works with vcpkg and gRPC's own CMake install)
find_package(gRPC CONFIG QUIET)
find_package(protobuf CONFIG QUIET)

if(gRPC_FOUND AND protobuf_FOUND)
    message(STATUS "Found gRPC and protobuf via CMake CONFIG")
    # Prefer system PATH protoc over vcpkg protoc (vcpkg's may be blocked
    # by Windows Application Control / Device Guard). The system protoc
    # must match the vcpkg protobuf library version.
    find_program(_SYSTEM_PROTOC protoc NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH)
    if(_SYSTEM_PROTOC)
        set(_PROTOC "${_SYSTEM_PROTOC}")
        message(STATUS "Using system protoc: ${_SYSTEM_PROTOC}")
    else()
        set(_PROTOC $<TARGET_FILE:protobuf::protoc>)
    endif()
    set(_GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
    set(_GRPC_LIBS gRPC::grpc++ protobuf::libprotobuf)
else()
    # Try pkg-config (Linux system packages)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PROTOBUF QUIET IMPORTED_TARGET protobuf)
        pkg_check_modules(GRPC QUIET IMPORTED_TARGET grpc++)
    endif()

    if(PROTOBUF_FOUND AND GRPC_FOUND)
        message(STATUS "Found gRPC and protobuf via pkg-config")
        find_program(_PROTOC protoc REQUIRED)
        find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
        set(_GRPC_LIBS PkgConfig::GRPC PkgConfig::PROTOBUF)
    else()
        # FetchContent as last resort
        message(STATUS "gRPC not found locally — fetching from source (this will take a while)")
        include(FetchContent)

        set(FETCHCONTENT_QUIET OFF)
        set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
        set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)

        # CMake 4.x rejects cmake_minimum_required < 3.5 in bundled deps.
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

        # Suppress install() errors from gRPC's bundled protobuf + abseil
        # when built via FetchContent (export sets reference each other).
        set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
        set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
        set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(
            grpc
            GIT_REPOSITORY https://github.com/grpc/grpc.git
            GIT_TAG        v1.78.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(grpc)

        set(_PROTOC $<TARGET_FILE:protoc>)
        set(_GRPC_CPP_PLUGIN $<TARGET_FILE:grpc_cpp_plugin>)
        set(_GRPC_LIBS grpc++ libprotobuf)
    endif()
endif()

# --- Generate C++ code ---

set(PROTO_GENERATED_SRCS
    "${PROTO_OUT}/gpu_share.pb.cc"
    "${PROTO_OUT}/gpu_share.grpc.pb.cc"
)
set(PROTO_GENERATED_HDRS
    "${PROTO_OUT}/gpu_share.pb.h"
    "${PROTO_OUT}/gpu_share.grpc.pb.h"
)

add_custom_command(
    OUTPUT ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS}
    COMMAND ${_PROTOC}
        --proto_path="${CMAKE_CURRENT_SOURCE_DIR}"
        --cpp_out="${PROTO_OUT}"
        --grpc_out="${PROTO_OUT}"
        --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
        "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}"
    COMMENT "Generating C++ protobuf/gRPC code from gpu_share.proto"
)

add_library(gpu_share_proto STATIC ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS})

target_include_directories(gpu_share_proto PUBLIC "${PROTO_OUT}")

target_link_libraries(gpu_share_proto PUBLIC ${_GRPC_LIBS})

# Export protoc path for use by deno_renderer's cargo build
set(GPU_SHARE_PROTOC "${_PROTOC}" PARENT_SCOPE)
