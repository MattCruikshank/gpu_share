#ifndef _WIN32

#include "handle_transport.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

class UnixHandleTransport : public HandleTransport {
public:
    ~UnixHandleTransport() override { close(); }

    bool listen(const std::string& endpoint) override {
        listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            fprintf(stderr, "UnixHandleTransport::listen: socket() failed: %s\n",
                    strerror(errno));
            return false;
        }

        endpoint_ = endpoint;
        isServer_ = true;

        // Remove any existing socket file.
        unlink(endpoint.c_str());

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            fprintf(stderr, "UnixHandleTransport::listen: bind(%s) failed: %s\n",
                    endpoint.c_str(), strerror(errno));
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        if (::listen(listenFd_, 1) < 0) {
            fprintf(stderr, "UnixHandleTransport::listen: listen() failed: %s\n",
                    strerror(errno));
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }

        return true;
    }

    bool accept() override {
        if (listenFd_ < 0) {
            fprintf(stderr, "UnixHandleTransport::accept: not listening\n");
            return false;
        }

        connFd_ = ::accept(listenFd_, nullptr, nullptr);
        if (connFd_ < 0) {
            fprintf(stderr, "UnixHandleTransport::accept: accept() failed: %s\n",
                    strerror(errno));
            return false;
        }

        return true;
    }

    bool connect(const std::string& endpoint) override {
        connFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connFd_ < 0) {
            fprintf(stderr, "UnixHandleTransport::connect: socket() failed: %s\n",
                    strerror(errno));
            return false;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(connFd_, reinterpret_cast<struct sockaddr*>(&addr),
                      sizeof(addr)) < 0) {
            fprintf(stderr, "UnixHandleTransport::connect: connect(%s) failed: %s\n",
                    endpoint.c_str(), strerror(errno));
            ::close(connFd_);
            connFd_ = -1;
            return false;
        }

        return true;
    }

    bool sendHandle(SharedMemoryHandle handle,
                    const void* data, size_t dataLen) override {
        if (connFd_ < 0) {
            fprintf(stderr, "UnixHandleTransport::sendHandle: not connected\n");
            return false;
        }

        // iov carries the data payload (or a dummy byte if no payload).
        char dummy = 0;
        struct iovec iov{};
        if (data && dataLen > 0) {
            iov.iov_base = const_cast<void*>(data);
            iov.iov_len = dataLen;
        } else {
            iov.iov_base = &dummy;
            iov.iov_len = 1;
        }

        // Build the message with SCM_RIGHTS ancillary data.
        union {
            char buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } cmsgBuf{};

        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgBuf.buf;
        msg.msg_controllen = sizeof(cmsgBuf.buf);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &handle, sizeof(int));

        ssize_t sent = sendmsg(connFd_, &msg, 0);
        if (sent < 0) {
            fprintf(stderr, "UnixHandleTransport::sendHandle: sendmsg() failed: %s\n",
                    strerror(errno));
            return false;
        }

        return true;
    }

    bool recvHandle(SharedMemoryHandle& outHandle,
                    void* data, size_t dataLen) override {
        if (connFd_ < 0) {
            fprintf(stderr, "UnixHandleTransport::recvHandle: not connected\n");
            return false;
        }

        outHandle = kInvalidMemoryHandle;

        // iov receives the data payload (or a dummy byte if no payload).
        char dummy = 0;
        struct iovec iov{};
        if (data && dataLen > 0) {
            iov.iov_base = data;
            iov.iov_len = dataLen;
        } else {
            iov.iov_base = &dummy;
            iov.iov_len = 1;
        }

        union {
            char buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } cmsgBuf{};

        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgBuf.buf;
        msg.msg_controllen = sizeof(cmsgBuf.buf);

        ssize_t received = recvmsg(connFd_, &msg, 0);
        if (received < 0) {
            fprintf(stderr, "UnixHandleTransport::recvHandle: recvmsg() failed: %s\n",
                    strerror(errno));
            return false;
        }
        if (received == 0) {
            fprintf(stderr, "UnixHandleTransport::recvHandle: connection closed\n");
            return false;
        }

        // Extract the fd from ancillary data.
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
            fprintf(stderr, "UnixHandleTransport::recvHandle: no SCM_RIGHTS in message\n");
            return false;
        }

        memcpy(&outHandle, CMSG_DATA(cmsg), sizeof(int));
        return true;
    }

    bool sendData(const void* data, size_t len) override {
        if (connFd_ < 0) return false;
        const char* ptr = static_cast<const char*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            ssize_t n = ::send(connFd_, ptr, remaining, 0);
            if (n <= 0) return false;
            ptr += n;
            remaining -= n;
        }
        return true;
    }

    bool recvDataNonBlocking(void* data, size_t maxLen, size_t& outLen) override {
        outLen = 0;
        if (connFd_ < 0) return false;
        ssize_t n = ::recv(connFd_, data, maxLen, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
            return false;
        }
        if (n == 0) return false;
        outLen = static_cast<size_t>(n);
        return true;
    }

    void close() override {
        if (connFd_ >= 0) {
            ::close(connFd_);
            connFd_ = -1;
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (isServer_ && !endpoint_.empty()) {
            unlink(endpoint_.c_str());
            endpoint_.clear();
            isServer_ = false;
        }
    }

private:
    int listenFd_ = -1;
    int connFd_ = -1;
    bool isServer_ = false;
    std::string endpoint_;
};

std::unique_ptr<HandleTransport> HandleTransport::create() {
    return std::make_unique<UnixHandleTransport>();
}

#endif // !_WIN32
