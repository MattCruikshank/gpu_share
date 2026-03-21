# Generate C++ code from gpu_share.proto (protobuf + gRPC)
# Uses system-installed protobuf + gRPC packages.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)

find_program(PROTOC protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/gpu_share.proto")
set(PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}")

# Generated file paths
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
    COMMAND ${PROTOC}
        --proto_path="${CMAKE_CURRENT_SOURCE_DIR}"
        --cpp_out="${PROTO_OUT}"
        --grpc_out="${PROTO_OUT}"
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}"
    COMMENT "Generating C++ protobuf/gRPC code from gpu_share.proto"
)

add_library(gpu_share_proto STATIC ${PROTO_GENERATED_SRCS} ${PROTO_GENERATED_HDRS})

target_include_directories(gpu_share_proto PUBLIC "${PROTO_OUT}")

target_link_libraries(gpu_share_proto PUBLIC
    PkgConfig::GRPC
    PkgConfig::PROTOBUF
)
