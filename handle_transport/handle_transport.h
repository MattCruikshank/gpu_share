#pragma once

#include "../shared/shared_handle.h"
#include <string>
#include <memory>

// Cross-platform transport for passing GPU memory handles between processes.
// Uses TCP 127.0.0.1:<port> on all platforms. Handle passing:
// Linux:   pidfd_open + pidfd_getfd (kernel 5.6+)
// Windows: OpenProcess + DuplicateHandle
class HandleTransport {
public:
    virtual ~HandleTransport() = default;

    // Server side
    virtual bool listen(const std::string& endpoint) = 0;
    virtual bool accept() = 0;  // blocks

    // Client side
    virtual bool connect(const std::string& endpoint) = 0;

    // Transfer a memory handle with an arbitrary data payload.
    // On Linux: fd is sent via SCM_RIGHTS ancillary data.
    // On Windows: HANDLE is duplicated into the remote process, then the
    //             duplicated handle value is sent over the named pipe.
    virtual bool sendHandle(SharedMemoryHandle handle,
                            const void* data = nullptr, size_t dataLen = 0) = 0;
    virtual bool recvHandle(SharedMemoryHandle& outHandle,
                            void* data = nullptr, size_t dataLen = 0) = 0;

    // Plain data send/receive (no handle passing, reuses the same connection).
    virtual bool sendData(const void* data, size_t len) = 0;
    // Returns false if no data available (non-blocking) or error.
    // Sets outLen to bytes actually read.
    virtual bool recvDataNonBlocking(void* data, size_t maxLen, size_t& outLen) = 0;

    // Non-blocking version of recvHandle. Returns false if nothing available.
    virtual bool recvHandleNonBlocking(SharedMemoryHandle& outHandle,
                                       void* data, size_t dataLen) = 0;

    virtual void close() = 0;

    // Cancel a blocking accept(). Safe to call from a signal/ctrl handler.
    virtual void cancel() {}

    // Factory: returns platform-appropriate implementation.
    static std::unique_ptr<HandleTransport> create();
};
