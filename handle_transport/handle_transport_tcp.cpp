// WIN32_LEAN_AND_MEAN must come before any Windows header to prevent
// windows.h from pulling in winsock.h (which conflicts with winsock2.h).
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#endif

#include "handle_transport.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <memory>

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <poll.h>
  #include <sys/syscall.h>
  #include <fcntl.h>
  #include <sys/ioctl.h>
  #include <cerrno>
#endif

// ---------------------------------------------------------------------------
// Platform abstractions
// ---------------------------------------------------------------------------

#ifdef _WIN32
using SocketType = SOCKET;
constexpr SocketType kInvalidSocket = INVALID_SOCKET;
#else
using SocketType = int;
constexpr SocketType kInvalidSocket = -1;
#endif

// Linux pidfd syscall numbers (in case headers are too old)
#ifndef _WIN32
  #ifndef SYS_pidfd_open
    #define SYS_pidfd_open 434
  #endif
  #ifndef SYS_pidfd_getfd
    #define SYS_pidfd_getfd 438
  #endif
#endif

// ---------------------------------------------------------------------------
// Winsock initialization (Windows only)
// ---------------------------------------------------------------------------

#ifdef _WIN32
static void ensureWinsockInit() {
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
}
#endif

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

static void closeSocket(SocketType& s) {
    if (s == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
    s = kInvalidSocket;
}

static bool setSocketNodelay(SocketType s) {
    int flag = 1;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&flag),
                      sizeof(flag)) == 0;
}

static struct sockaddr_in makeAddr(uint16_t port) {
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

// ---------------------------------------------------------------------------
// TCP read/write helpers (handle partial reads/writes)
// ---------------------------------------------------------------------------

static bool tcpWriteAll(SocketType s, const void* data, size_t size) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = size;
    while (remaining > 0) {
#ifdef _WIN32
        int n = ::send(s, ptr, static_cast<int>(remaining), 0);
#else
        ssize_t n = ::send(s, ptr, remaining, 0);
#endif
        if (n <= 0) {
            fprintf(stderr, "tcpWriteAll: send failed\n");
            return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

static bool tcpReadAll(SocketType s, void* data, size_t size) {
    char* ptr = static_cast<char*>(data);
    size_t remaining = size;
    while (remaining > 0) {
#ifdef _WIN32
        int n = ::recv(s, ptr, static_cast<int>(remaining), 0);
#else
        ssize_t n = ::recv(s, ptr, remaining, 0);
#endif
        if (n <= 0) {
            fprintf(stderr, "tcpReadAll: recv failed\n");
            return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// TcpHandleTransport
// ---------------------------------------------------------------------------

class TcpHandleTransport : public HandleTransport {
public:
    ~TcpHandleTransport() override { close(); }

    // -----------------------------------------------------------------------
    // Server side
    // -----------------------------------------------------------------------

    bool listen(const std::string& endpoint) override {
#ifdef _WIN32
        ensureWinsockInit();
#endif
        uint16_t port = static_cast<uint16_t>(std::stoi(endpoint));

        listenSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock_ == kInvalidSocket) {
            fprintf(stderr, "TcpHandleTransport::listen: socket() failed\n");
            return false;
        }

        // Allow address reuse.
        int reuse = 1;
        setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        struct sockaddr_in addr = makeAddr(port);
        if (::bind(listenSock_, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)) != 0) {
            fprintf(stderr, "TcpHandleTransport::listen: bind(port %u) failed\n",
                    port);
            closeSocket(listenSock_);
            return false;
        }

        if (::listen(listenSock_, 1) != 0) {
            fprintf(stderr, "TcpHandleTransport::listen: listen() failed\n");
            closeSocket(listenSock_);
            return false;
        }

        isServer_ = true;

        // Create cancel mechanism.
#ifdef _WIN32
        cancelEvent_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
#else
        if (::pipe(cancelPipe_) < 0) {
            fprintf(stderr, "TcpHandleTransport::listen: pipe() failed\n");
            closeSocket(listenSock_);
            return false;
        }
#endif

        return true;
    }

    bool accept() override {
        if (listenSock_ == kInvalidSocket) {
            fprintf(stderr, "TcpHandleTransport::accept: not listening\n");
            return false;
        }

#ifdef _WIN32
        // Poll for incoming connections with cancel support.
        // A connection may already be queued in the backlog if the client
        // connected before we got here, so check non-blocking first.
        {
            u_long nonBlock = 1;
            ioctlsocket(listenSock_, FIONBIO, &nonBlock);

            while (true) {
                connSock_ = ::accept(listenSock_, nullptr, nullptr);
                if (connSock_ != kInvalidSocket) break;

                // Check if cancelled
                if (cancelEvent_ &&
                    WaitForSingleObject(cancelEvent_, 0) == WAIT_OBJECT_0) {
                    fprintf(stderr, "TcpHandleTransport::accept: cancelled\n");
                    u_long blocking = 0;
                    ioctlsocket(listenSock_, FIONBIO, &blocking);
                    return false;
                }

                Sleep(10);
            }

            u_long blocking = 0;
            ioctlsocket(listenSock_, FIONBIO, &blocking);
        }
#else
        // Use poll() on both listenSock_ and cancelPipe_[0].
        struct pollfd fds[2];
        fds[0].fd = listenSock_;
        fds[0].events = POLLIN;
        fds[1].fd = cancelPipe_[0];
        fds[1].events = POLLIN;

        int ret = ::poll(fds, 2, -1);
        if (ret < 0) {
            fprintf(stderr, "TcpHandleTransport::accept: poll() failed: %s\n",
                    strerror(errno));
            return false;
        }
        if (fds[1].revents & POLLIN) {
            fprintf(stderr, "TcpHandleTransport::accept: cancelled\n");
            return false;
        }

        connSock_ = ::accept(listenSock_, nullptr, nullptr);
        if (connSock_ == kInvalidSocket) {
            fprintf(stderr, "TcpHandleTransport::accept: accept() failed: %s\n",
                    strerror(errno));
            return false;
        }
#endif

        setSocketNodelay(connSock_);
        return exchangePids();
    }

    // -----------------------------------------------------------------------
    // Client side
    // -----------------------------------------------------------------------

    bool connect(const std::string& endpoint) override {
#ifdef _WIN32
        ensureWinsockInit();
#endif
        uint16_t port = static_cast<uint16_t>(std::stoi(endpoint));

        connSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connSock_ == kInvalidSocket) {
            fprintf(stderr, "TcpHandleTransport::connect: socket() failed\n");
            return false;
        }

        struct sockaddr_in addr = makeAddr(port);
        if (::connect(connSock_, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) != 0) {
            fprintf(stderr, "TcpHandleTransport::connect: connect(port %u) failed\n",
                    port);
            closeSocket(connSock_);
            return false;
        }

        setSocketNodelay(connSock_);
        isServer_ = false;
        return exchangePids();
    }

    // -----------------------------------------------------------------------
    // Handle transfer
    // -----------------------------------------------------------------------

    bool sendHandle(SharedMemoryHandle handle,
                    const void* data, size_t dataLen) override {
        if (connSock_ == kInvalidSocket) {
            fprintf(stderr, "TcpHandleTransport::sendHandle: not connected\n");
            return false;
        }

#ifdef _WIN32
        if (remoteProcess_ == nullptr) {
            fprintf(stderr, "TcpHandleTransport::sendHandle: "
                    "remote process handle not available\n");
            return false;
        }

        // Duplicate the handle into the remote process.
        HANDLE remoteHandle = nullptr;
        if (!DuplicateHandle(
                GetCurrentProcess(), handle,
                remoteProcess_, &remoteHandle,
                0, FALSE, DUPLICATE_SAME_ACCESS)) {
            fprintf(stderr, "TcpHandleTransport::sendHandle: DuplicateHandle "
                    "failed, error %lu\n", GetLastError());
            return false;
        }

        uint64_t handleValue = reinterpret_cast<uint64_t>(remoteHandle);
#else
        // On Linux, just send the raw fd value; receiver uses pidfd_getfd.
        uint64_t handleValue = static_cast<uint64_t>(handle);
#endif

        if (!tcpWriteAll(connSock_, &handleValue, sizeof(handleValue))) {
            fprintf(stderr, "TcpHandleTransport::sendHandle: "
                    "failed to write handle value\n");
            return false;
        }

        if (data && dataLen > 0) {
            if (!tcpWriteAll(connSock_, data, dataLen)) {
                fprintf(stderr, "TcpHandleTransport::sendHandle: "
                        "failed to write data payload\n");
                return false;
            }
        }

        return true;
    }

    bool recvHandle(SharedMemoryHandle& outHandle,
                    void* data, size_t dataLen) override {
        if (connSock_ == kInvalidSocket) {
            fprintf(stderr, "TcpHandleTransport::recvHandle: not connected\n");
            return false;
        }

        outHandle = kInvalidMemoryHandle;

        uint64_t handleValue = 0;
        if (!tcpReadAll(connSock_, &handleValue, sizeof(handleValue))) {
            fprintf(stderr, "TcpHandleTransport::recvHandle: "
                    "failed to read handle value\n");
            return false;
        }

        if (data && dataLen > 0) {
            if (!tcpReadAll(connSock_, data, dataLen)) {
                fprintf(stderr, "TcpHandleTransport::recvHandle: "
                        "failed to read data payload\n");
                return false;
            }
        }

#ifdef _WIN32
        // The handle was duplicated into our process by the sender.
        outHandle = reinterpret_cast<HANDLE>(handleValue);
#else
        // Clone the remote fd into our process via pidfd_getfd.
        if (pidfd_ < 0) {
            fprintf(stderr, "TcpHandleTransport::recvHandle: pidfd not available\n");
            return false;
        }
        int localFd = static_cast<int>(
            syscall(SYS_pidfd_getfd, pidfd_, static_cast<int>(handleValue), 0));
        if (localFd < 0) {
            fprintf(stderr, "TcpHandleTransport::recvHandle: pidfd_getfd failed: %s\n",
                    strerror(errno));
            return false;
        }
        outHandle = localFd;
#endif

        return true;
    }

    bool recvHandleNonBlocking(SharedMemoryHandle& outHandle,
                                void* data, size_t dataLen) override {
        outHandle = kInvalidMemoryHandle;
        if (connSock_ == kInvalidSocket) return false;

        size_t needed = sizeof(uint64_t) + dataLen;

#ifdef _WIN32
        // Check available bytes via ioctlsocket.
        u_long available = 0;
        if (ioctlsocket(connSock_, FIONREAD, &available) != 0) return false;
        if (static_cast<size_t>(available) < needed) return false;
#else
        // Peek to see if enough bytes are available.
        // Use a small stack buffer for the peek (we only need to know the count).
        char peekBuf[8];
        ssize_t peeked = ::recv(connSock_, peekBuf,
                                needed < sizeof(peekBuf) ? needed : sizeof(peekBuf),
                                MSG_PEEK | MSG_DONTWAIT);
        if (peeked < 0) return false;
        if (static_cast<size_t>(peeked) < needed) {
            // Not enough bytes yet. But if we peeked fewer than our buffer size
            // AND fewer than needed, not enough data. If we peeked our full
            // buffer but needed > buffer, we need a different check.
            // Use ioctl FIONREAD for accurate count.
            int avail = 0;
            if (ioctl(connSock_, FIONREAD, &avail) < 0) return false;
            if (static_cast<size_t>(avail) < needed) return false;
        }
#endif

        // Enough data is available — do a blocking read of the full payload.
        uint64_t handleValue = 0;
        if (!tcpReadAll(connSock_, &handleValue, sizeof(handleValue))) return false;

        if (data && dataLen > 0) {
            if (!tcpReadAll(connSock_, data, dataLen)) return false;
        }

#ifdef _WIN32
        outHandle = reinterpret_cast<HANDLE>(handleValue);
#else
        if (pidfd_ < 0) return false;
        int localFd = static_cast<int>(
            syscall(SYS_pidfd_getfd, pidfd_, static_cast<int>(handleValue), 0));
        if (localFd < 0) return false;
        outHandle = localFd;
#endif

        return true;
    }

    // -----------------------------------------------------------------------
    // Plain data send/receive
    // -----------------------------------------------------------------------

    bool sendData(const void* data, size_t len) override {
        if (connSock_ == kInvalidSocket) return false;
        return tcpWriteAll(connSock_, data, len);
    }

    bool recvDataNonBlocking(void* data, size_t maxLen, size_t& outLen) override {
        outLen = 0;
        if (connSock_ == kInvalidSocket) return false;

#ifdef _WIN32
        u_long available = 0;
        if (ioctlsocket(connSock_, FIONREAD, &available) != 0) return false;
        if (available == 0) return false;

        int toRead = (static_cast<size_t>(available) < maxLen)
                      ? static_cast<int>(available)
                      : static_cast<int>(maxLen);
        int n = ::recv(connSock_, static_cast<char*>(data), toRead, 0);
        if (n <= 0) return false;
        outLen = static_cast<size_t>(n);
        return true;
#else
        ssize_t n = ::recv(connSock_, data, maxLen, MSG_DONTWAIT);
        if (n < 0) return false;  // EAGAIN/EWOULDBLOCK or error
        if (n == 0) return false; // connection closed
        outLen = static_cast<size_t>(n);
        return true;
#endif
    }

    // -----------------------------------------------------------------------
    // cancel / close
    // -----------------------------------------------------------------------

    void cancel() override {
#ifdef _WIN32
        if (cancelEvent_) SetEvent(cancelEvent_);
#else
        if (cancelPipe_[1] >= 0) {
            char c = 1;
            (void)::write(cancelPipe_[1], &c, 1);
        }
#endif
    }

    void close() override {
        closeSocket(connSock_);
        closeSocket(listenSock_);

#ifdef _WIN32
        if (remoteProcess_ != nullptr) {
            CloseHandle(remoteProcess_);
            remoteProcess_ = nullptr;
        }
        if (cancelEvent_ != nullptr) {
            CloseHandle(cancelEvent_);
            cancelEvent_ = nullptr;
        }
#else
        if (pidfd_ >= 0) {
            ::close(pidfd_);
            pidfd_ = -1;
        }
        if (cancelPipe_[0] >= 0) {
            ::close(cancelPipe_[0]);
            cancelPipe_[0] = -1;
        }
        if (cancelPipe_[1] >= 0) {
            ::close(cancelPipe_[1]);
            cancelPipe_[1] = -1;
        }
#endif

        isServer_ = false;
    }

private:
    // -----------------------------------------------------------------------
    // PID exchange
    // -----------------------------------------------------------------------

    bool exchangePids() {
#ifdef _WIN32
        DWORD myPid = GetCurrentProcessId();
        DWORD remotePid = 0;
#else
        uint32_t myPid = static_cast<uint32_t>(getpid());
        uint32_t remotePid = 0;
#endif

        if (isServer_) {
            // Server sends its PID first, then reads the client's.
            if (!tcpWriteAll(connSock_, &myPid, sizeof(myPid))) {
                fprintf(stderr, "TcpHandleTransport::exchangePids: "
                        "failed to send PID\n");
                return false;
            }
            if (!tcpReadAll(connSock_, &remotePid, sizeof(remotePid))) {
                fprintf(stderr, "TcpHandleTransport::exchangePids: "
                        "failed to receive remote PID\n");
                return false;
            }
        } else {
            // Client reads the server's PID first, then sends its own.
            if (!tcpReadAll(connSock_, &remotePid, sizeof(remotePid))) {
                fprintf(stderr, "TcpHandleTransport::exchangePids: "
                        "failed to receive remote PID\n");
                return false;
            }
            if (!tcpWriteAll(connSock_, &myPid, sizeof(myPid))) {
                fprintf(stderr, "TcpHandleTransport::exchangePids: "
                        "failed to send PID\n");
                return false;
            }
        }

#ifdef _WIN32
        remoteProcess_ = OpenProcess(PROCESS_DUP_HANDLE, FALSE, remotePid);
        if (remoteProcess_ == nullptr) {
            fprintf(stderr, "TcpHandleTransport::exchangePids: "
                    "OpenProcess(%lu) failed, error %lu\n",
                    static_cast<unsigned long>(remotePid), GetLastError());
            return false;
        }
#else
        pidfd_ = static_cast<int>(
            syscall(SYS_pidfd_open, static_cast<pid_t>(remotePid), 0));
        if (pidfd_ < 0) {
            fprintf(stderr, "TcpHandleTransport::exchangePids: "
                    "pidfd_open(%u) failed: %s\n", remotePid, strerror(errno));
            return false;
        }
#endif

        return true;
    }

    // -----------------------------------------------------------------------
    // Member variables
    // -----------------------------------------------------------------------

    SocketType listenSock_ = kInvalidSocket;
    SocketType connSock_   = kInvalidSocket;
    bool isServer_ = false;

#ifdef _WIN32
    HANDLE remoteProcess_ = nullptr;
    HANDLE cancelEvent_   = nullptr;
#else
    int pidfd_ = -1;
    int cancelPipe_[2] = { -1, -1 };
#endif
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<HandleTransport> HandleTransport::create() {
    return std::make_unique<TcpHandleTransport>();
}
