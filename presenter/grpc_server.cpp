#include "grpc_server.h"

#include <grpcpp/grpcpp.h>
#include "gpu_share.grpc.pb.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <cstdio>
#include <atomic>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
  #include <sys/syscall.h>
  #include <cerrno>
  #include <cstring>

  #ifndef SYS_pidfd_open
    #define SYS_pidfd_open 434
  #endif
  #ifndef SYS_pidfd_getfd
    #define SYS_pidfd_getfd 438
  #endif
#endif

// ---------------------------------------------------------------------------
// fd cloning utility
// ---------------------------------------------------------------------------

static SharedMemoryHandle cloneHandle(uint32_t remotePid, uint64_t remoteHandle) {
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, remotePid);
    if (!hProcess) {
        fprintf(stderr, "[grpc_server] OpenProcess(%u) failed: %lu\n",
                remotePid, GetLastError());
        return kInvalidMemoryHandle;
    }
    HANDLE localHandle = nullptr;
    BOOL ok = DuplicateHandle(hProcess, reinterpret_cast<HANDLE>(remoteHandle),
                              GetCurrentProcess(), &localHandle,
                              0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(hProcess);
    if (!ok) {
        fprintf(stderr, "[grpc_server] DuplicateHandle failed: %lu\n", GetLastError());
        return kInvalidMemoryHandle;
    }
    return localHandle;
#else
    int pidfd = static_cast<int>(syscall(SYS_pidfd_open, static_cast<pid_t>(remotePid), 0));
    if (pidfd < 0) {
        fprintf(stderr, "[grpc_server] pidfd_open(%u) failed: %s\n",
                remotePid, strerror(errno));
        return kInvalidMemoryHandle;
    }
    int localFd = static_cast<int>(
        syscall(SYS_pidfd_getfd, pidfd, static_cast<int>(remoteHandle), 0));
    close(pidfd);
    if (localFd < 0) {
        fprintf(stderr, "[grpc_server] pidfd_getfd failed: %s\n", strerror(errno));
        return kInvalidMemoryHandle;
    }
    return localFd;
#endif
}

static SharedSurfaceInfo protoToSurfaceInfo(const gpu_share::SharedSurfaceInfo& p) {
    SharedSurfaceInfo info{};
    info.width = p.width();
    info.height = p.height();
    info.format = static_cast<VkFormat>(p.format());
    info.memorySize = p.memory_size();
    info.memoryTypeBits = p.memory_type_bits();
    return info;
}

// ---------------------------------------------------------------------------
// Shared state between gRPC service handlers and the main loop
// ---------------------------------------------------------------------------

struct BridgeState {
    // --- Connect handshake ---
    std::mutex connectMu;
    std::condition_variable connectCv;
    bool rendererConnected = false;
    uint32_t rendererPid = 0;
    SharedMemoryHandle memoryHandle = kInvalidMemoryHandle;
    SharedSurfaceInfo surfaceInfo{};

    // --- Input event queue (main loop → StreamInput handler) ---
    std::mutex eventMu;
    std::condition_variable eventCv;
    std::queue<gpu_share::InputEvent> eventQueue;
    bool shutdownEvents = false;

    // --- Surface update queue (NotifySurface handler → main loop) ---
    std::mutex surfaceMu;
    std::queue<std::pair<SharedMemoryHandle, SharedSurfaceInfo>> surfaceUpdates;
};

// ---------------------------------------------------------------------------
// gRPC service implementation
// ---------------------------------------------------------------------------

class RendererBridgeServiceImpl final : public gpu_share::RendererBridge::Service {
public:
    explicit RendererBridgeServiceImpl(BridgeState* state) : state_(state) {}

    grpc::Status RegisterSurface(grpc::ServerContext* /*ctx*/,
                                const gpu_share::ConnectRequest* request,
                                gpu_share::ConnectResponse* response) override {
        uint32_t remotePid = request->renderer_pid();
        uint64_t remoteHandleVal = request->memory_handle();

        fprintf(stderr, "[grpc_server] Connect from renderer pid=%u\n", remotePid);

        SharedMemoryHandle localHandle = cloneHandle(remotePid, remoteHandleVal);
        if (localHandle == kInvalidMemoryHandle) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to clone memory handle");
        }

        SharedSurfaceInfo info{};
        if (request->has_surface()) {
            info = protoToSurfaceInfo(request->surface());
        }

#ifdef _WIN32
        response->set_presenter_pid(GetCurrentProcessId());
#else
        response->set_presenter_pid(static_cast<uint32_t>(getpid()));
#endif

        {
            std::lock_guard<std::mutex> lk(state_->connectMu);
            state_->rendererPid = remotePid;
            state_->memoryHandle = localHandle;
            state_->surfaceInfo = info;
            state_->rendererConnected = true;
        }
        state_->connectCv.notify_one();

        fprintf(stderr, "[grpc_server] Connect complete: surface %ux%u\n",
                info.width, info.height);
        return grpc::Status::OK;
    }

    grpc::Status StreamInput(grpc::ServerContext* ctx,
                             const gpu_share::StreamInputRequest* /*request*/,
                             grpc::ServerWriter<gpu_share::InputEvent>* writer) override {
        fprintf(stderr, "[grpc_server] StreamInput started\n");

        while (!ctx->IsCancelled()) {
            gpu_share::InputEvent event;
            {
                std::unique_lock<std::mutex> lk(state_->eventMu);
                state_->eventCv.wait_for(lk, std::chrono::milliseconds(100), [this] {
                    return !state_->eventQueue.empty() || state_->shutdownEvents;
                });

                if (state_->shutdownEvents) break;
                if (state_->eventQueue.empty()) continue;

                event = std::move(state_->eventQueue.front());
                state_->eventQueue.pop();
            }

            // fprintf(stderr, "[grpc_server] StreamInput: sending event (case=%d)\n",
            //         event.event_case());
            if (!writer->Write(event)) {
                fprintf(stderr, "[grpc_server] StreamInput: write failed (client disconnected?)\n");
                break;
            }
        }

        fprintf(stderr, "[grpc_server] StreamInput ended\n");
        return grpc::Status::OK;
    }

    grpc::Status NotifySurface(grpc::ServerContext* /*ctx*/,
                               const gpu_share::SurfaceUpdate* request,
                               gpu_share::SurfaceAck* /*response*/) override {
        uint32_t rendererPid;
        {
            std::lock_guard<std::mutex> lk(state_->connectMu);
            rendererPid = state_->rendererPid;
        }

        SharedMemoryHandle localHandle = cloneHandle(rendererPid, request->memory_handle());
        if (localHandle == kInvalidMemoryHandle) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to clone handle on resize");
        }

        SharedSurfaceInfo info{};
        if (request->has_surface()) {
            info = protoToSurfaceInfo(request->surface());
        }

        {
            std::lock_guard<std::mutex> lk(state_->surfaceMu);
            state_->surfaceUpdates.push({localHandle, info});
        }

        fprintf(stderr, "[grpc_server] NotifySurface: %ux%u\n", info.width, info.height);
        return grpc::Status::OK;
    }

private:
    BridgeState* state_;
};

// ---------------------------------------------------------------------------
// GrpcBridgeImpl
// ---------------------------------------------------------------------------

struct GrpcBridgeImpl {
    BridgeState state;
    std::unique_ptr<grpc::Server> server;
    std::thread serverThread;
};

// ---------------------------------------------------------------------------
// GrpcBridge public API
// ---------------------------------------------------------------------------

GrpcBridge::GrpcBridge() : impl_(std::make_unique<GrpcBridgeImpl>()) {}
GrpcBridge::~GrpcBridge() { stop(); }

bool GrpcBridge::start(uint16_t port) {
    std::string addr = "0.0.0.0:" + std::to_string(port);

    auto service = std::make_unique<RendererBridgeServiceImpl>(&impl_->state);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(service.get());

    impl_->server = builder.BuildAndStart();
    if (!impl_->server) {
        fprintf(stderr, "[grpc_server] Failed to start on %s\n", addr.c_str());
        return false;
    }

    fprintf(stderr, "[grpc_server] Listening on %s\n", addr.c_str());

    // Run server on a background thread (Server::Wait blocks)
    impl_->serverThread = std::thread([srv = impl_->server.get(),
                                       svc = std::move(service)]() {
        srv->Wait();
        // svc destroyed here after server shuts down
    });

    return true;
}

void GrpcBridge::stop() {
    if (!impl_->server) return;

    // Signal StreamInput to stop
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.shutdownEvents = true;
    }
    impl_->state.eventCv.notify_all();

    impl_->server->Shutdown();
    if (impl_->serverThread.joinable()) {
        impl_->serverThread.join();
    }
    impl_->server.reset();
}

bool GrpcBridge::waitForRenderer(SharedMemoryHandle& outHandle,
                                  SharedSurfaceInfo& outInfo,
                                  uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lk(impl_->state.connectMu);
    if (!impl_->state.connectCv.wait_for(lk,
            std::chrono::milliseconds(timeoutMs),
            [this] { return impl_->state.rendererConnected; })) {
        fprintf(stderr, "[grpc_server] Timed out waiting for renderer\n");
        return false;
    }
    outHandle = impl_->state.memoryHandle;
    outInfo = impl_->state.surfaceInfo;
    return true;
}

void GrpcBridge::pushMouseMotion(float x, float y, float relX, float relY) {
    gpu_share::InputEvent ev;
    auto* m = ev.mutable_mouse_motion();
    m->set_x(x);
    m->set_y(y);
    m->set_rel_x(relX);
    m->set_rel_y(relY);
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.eventQueue.push(std::move(ev));
    }
    impl_->state.eventCv.notify_one();
}

void GrpcBridge::pushMouseButton(uint32_t button, bool pressed, float x, float y) {
    gpu_share::InputEvent ev;
    auto* m = ev.mutable_mouse_button();
    m->set_button(button);
    m->set_pressed(pressed);
    m->set_x(x);
    m->set_y(y);
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.eventQueue.push(std::move(ev));
    }
    impl_->state.eventCv.notify_one();
}

void GrpcBridge::pushMouseWheel(float scrollY) {
    gpu_share::InputEvent ev;
    ev.mutable_mouse_wheel()->set_scroll_y(scrollY);
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.eventQueue.push(std::move(ev));
    }
    impl_->state.eventCv.notify_one();
}

void GrpcBridge::pushKeyDown(uint32_t scancode) {
    gpu_share::InputEvent ev;
    ev.mutable_key_down()->set_scancode(scancode);
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.eventQueue.push(std::move(ev));
    }
    impl_->state.eventCv.notify_one();
}

void GrpcBridge::pushKeyUp(uint32_t scancode) {
    gpu_share::InputEvent ev;
    ev.mutable_key_up()->set_scancode(scancode);
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.eventQueue.push(std::move(ev));
    }
    impl_->state.eventCv.notify_one();
}

void GrpcBridge::pushResize(uint32_t width, uint32_t height) {
    gpu_share::InputEvent ev;
    auto* r = ev.mutable_resize();
    r->set_width(width);
    r->set_height(height);
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.eventQueue.push(std::move(ev));
    }
    impl_->state.eventCv.notify_one();
}

void GrpcBridge::pushTabPause() {
    gpu_share::InputEvent ev;
    ev.mutable_tab_pause();
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.eventQueue.push(std::move(ev));
    }
    impl_->state.eventCv.notify_one();
}

void GrpcBridge::pushTabResume() {
    gpu_share::InputEvent ev;
    ev.mutable_tab_resume();
    {
        std::lock_guard<std::mutex> lk(impl_->state.eventMu);
        impl_->state.eventQueue.push(std::move(ev));
    }
    impl_->state.eventCv.notify_one();
}

bool GrpcBridge::pollSurfaceUpdate(SharedMemoryHandle& outHandle,
                                    SharedSurfaceInfo& outInfo) {
    std::lock_guard<std::mutex> lk(impl_->state.surfaceMu);
    if (impl_->state.surfaceUpdates.empty()) return false;
    auto [handle, info] = impl_->state.surfaceUpdates.front();
    impl_->state.surfaceUpdates.pop();
    outHandle = handle;
    outInfo = info;
    return true;
}
