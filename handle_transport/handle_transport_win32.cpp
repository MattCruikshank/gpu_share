#ifdef _WIN32

#include "handle_transport.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>

static bool writeAll(HANDLE pipe, const void* data, DWORD size) {
    const char* ptr = static_cast<const char*>(data);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, ptr, remaining, &written, nullptr)) {
            fprintf(stderr, "writeAll: WriteFile failed, error %lu\n",
                    GetLastError());
            return false;
        }
        ptr += written;
        remaining -= written;
    }
    return true;
}

static bool readAll(HANDLE pipe, void* data, DWORD size) {
    char* ptr = static_cast<char*>(data);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, ptr, remaining, &bytesRead, nullptr)) {
            fprintf(stderr, "readAll: ReadFile failed, error %lu\n",
                    GetLastError());
            return false;
        }
        if (bytesRead == 0) {
            fprintf(stderr, "readAll: pipe closed unexpectedly\n");
            return false;
        }
        ptr += bytesRead;
        remaining -= bytesRead;
    }
    return true;
}

static std::string toPipeName(const std::string& endpoint) {
    // If the endpoint already looks like a full pipe path, use it as-is.
    if (endpoint.size() >= 4 && endpoint.substr(0, 4) == "\\\\.\\") {
        return endpoint;
    }
    return "\\\\.\\pipe\\" + endpoint;
}

class Win32HandleTransport : public HandleTransport {
public:
    ~Win32HandleTransport() override { close(); }

    bool listen(const std::string& endpoint) override {
        std::string pipeName = toPipeName(endpoint);
        pipeName_ = pipeName;

        pipeHandle_ = CreateNamedPipeA(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,       // max instances
            4096,    // out buffer size
            4096,    // in buffer size
            0,       // default timeout
            nullptr  // security attributes
        );

        if (pipeHandle_ == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Win32HandleTransport::listen: CreateNamedPipeA(%s) "
                    "failed, error %lu\n", pipeName.c_str(), GetLastError());
            return false;
        }

        isServer_ = true;
        return true;
    }

    bool accept() override {
        if (pipeHandle_ == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Win32HandleTransport::accept: not listening\n");
            return false;
        }

        if (!ConnectNamedPipe(pipeHandle_, nullptr)) {
            DWORD err = GetLastError();
            // ERROR_PIPE_CONNECTED means the client connected between
            // CreateNamedPipe and ConnectNamedPipe, which is fine.
            if (err != ERROR_PIPE_CONNECTED) {
                fprintf(stderr, "Win32HandleTransport::accept: ConnectNamedPipe "
                        "failed, error %lu\n", err);
                return false;
            }
        }

        return exchangePids();
    }

    bool connect(const std::string& endpoint) override {
        std::string pipeName = toPipeName(endpoint);

        pipeHandle_ = CreateFileA(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,       // no sharing
            nullptr, // default security
            OPEN_EXISTING,
            0,       // default attributes
            nullptr  // no template
        );

        if (pipeHandle_ == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Win32HandleTransport::connect: CreateFileA(%s) "
                    "failed, error %lu\n", pipeName.c_str(), GetLastError());
            return false;
        }

        isServer_ = false;
        return exchangePids();
    }

    bool sendHandle(SharedMemoryHandle handle,
                    const void* data, size_t dataLen) override {
        if (pipeHandle_ == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Win32HandleTransport::sendHandle: not connected\n");
            return false;
        }

        if (remoteProcess_ == nullptr) {
            fprintf(stderr, "Win32HandleTransport::sendHandle: "
                    "remote process handle not available\n");
            return false;
        }

        // Duplicate the handle into the remote process.
        HANDLE remoteHandle = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(), handle,
                remoteProcess_, &remoteHandle,
                0, FALSE, DUPLICATE_SAME_ACCESS)) {
            fprintf(stderr, "Win32HandleTransport::sendHandle: DuplicateHandle "
                    "failed, error %lu\n", GetLastError());
            return false;
        }

        // Send the duplicated handle value as a uint64_t for consistent size.
        uint64_t handleValue = reinterpret_cast<uint64_t>(remoteHandle);
        if (!writeAll(pipeHandle_, &handleValue, sizeof(handleValue))) {
            fprintf(stderr, "Win32HandleTransport::sendHandle: "
                    "failed to write handle value\n");
            return false;
        }

        // Send the data payload.
        if (data && dataLen > 0) {
            if (!writeAll(pipeHandle_, data, static_cast<DWORD>(dataLen))) {
                fprintf(stderr, "Win32HandleTransport::sendHandle: "
                        "failed to write data payload\n");
                return false;
            }
        }

        return true;
    }

    bool recvHandle(SharedMemoryHandle& outHandle,
                    void* data, size_t dataLen) override {
        if (pipeHandle_ == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "Win32HandleTransport::recvHandle: not connected\n");
            return false;
        }

        outHandle = kInvalidMemoryHandle;

        // Read the handle value (8 bytes).
        uint64_t handleValue = 0;
        if (!readAll(pipeHandle_, &handleValue, sizeof(handleValue))) {
            fprintf(stderr, "Win32HandleTransport::recvHandle: "
                    "failed to read handle value\n");
            return false;
        }

        // The handle was duplicated into our process, so it is directly usable.
        outHandle = reinterpret_cast<HANDLE>(handleValue);

        // Read the data payload.
        if (data && dataLen > 0) {
            if (!readAll(pipeHandle_, data, static_cast<DWORD>(dataLen))) {
                fprintf(stderr, "Win32HandleTransport::recvHandle: "
                        "failed to read data payload\n");
                return false;
            }
        }

        return true;
    }

    void close() override {
        if (pipeHandle_ != INVALID_HANDLE_VALUE) {
            if (isServer_) {
                DisconnectNamedPipe(pipeHandle_);
            }
            CloseHandle(pipeHandle_);
            pipeHandle_ = INVALID_HANDLE_VALUE;
        }
        if (remoteProcess_ != nullptr) {
            CloseHandle(remoteProcess_);
            remoteProcess_ = nullptr;
        }
        isServer_ = false;
    }

private:
    // Exchange PIDs so both sides can open the remote process for
    // DuplicateHandle. The server sends first, then reads; the client
    // reads first, then sends -- avoiding deadlock.
    bool exchangePids() {
        DWORD myPid = GetCurrentProcessId();
        DWORD remotePid = 0;

        if (isServer_) {
            // Server sends its PID first, then reads the client's.
            if (!writeAll(pipeHandle_, &myPid, sizeof(myPid))) {
                fprintf(stderr, "Win32HandleTransport::exchangePids: "
                        "failed to send PID\n");
                return false;
            }
            if (!readAll(pipeHandle_, &remotePid, sizeof(remotePid))) {
                fprintf(stderr, "Win32HandleTransport::exchangePids: "
                        "failed to receive remote PID\n");
                return false;
            }
        } else {
            // Client reads the server's PID first, then sends its own.
            if (!readAll(pipeHandle_, &remotePid, sizeof(remotePid))) {
                fprintf(stderr, "Win32HandleTransport::exchangePids: "
                        "failed to receive remote PID\n");
                return false;
            }
            if (!writeAll(pipeHandle_, &myPid, sizeof(myPid))) {
                fprintf(stderr, "Win32HandleTransport::exchangePids: "
                        "failed to send PID\n");
                return false;
            }
        }

        remoteProcess_ = OpenProcess(PROCESS_DUP_HANDLE, FALSE, remotePid);
        if (remoteProcess_ == nullptr) {
            fprintf(stderr, "Win32HandleTransport::exchangePids: "
                    "OpenProcess(%lu) failed, error %lu\n",
                    remotePid, GetLastError());
            return false;
        }

        return true;
    }

    HANDLE pipeHandle_ = INVALID_HANDLE_VALUE;
    HANDLE remoteProcess_ = nullptr;
    bool isServer_ = false;
    std::string pipeName_;
};

std::unique_ptr<HandleTransport> HandleTransport::create() {
    return std::make_unique<Win32HandleTransport>();
}

#endif // _WIN32
