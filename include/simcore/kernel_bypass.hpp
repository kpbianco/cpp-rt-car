#pragma once
#include <string>
#ifdef __linux__
# include <sys/socket.h>
# include <unistd.h>
# if __has_include(<liburing.h>)
#  include <liburing.h>
#  define SIMCORE_HAS_IO_URING 1
# endif
#elif defined(_WIN32)
# include <winsock2.h>
# include <windows.h>
#endif

// Simple cross-platform socket wrapper enabling optional kernel bypass.
// On Linux io_uring is used when available. On Windows I/O completion ports
// are used.  When neither backend is available a blocking send() fallback is
// provided.  The poll() method enables DPDK-style busy polling when desired.
class KernelBypassSocket {
public:
    KernelBypassSocket();
    ~KernelBypassSocket();
    bool send(int fd, const std::string& data);
    void poll();
private:
#ifdef SIMCORE_HAS_IO_URING
    io_uring ring_{};
#elif defined(_WIN32)
    HANDLE iocp_{};
#endif
};

inline KernelBypassSocket::KernelBypassSocket() {
#ifdef SIMCORE_HAS_IO_URING
    io_uring_queue_init(8, &ring_, 0);
#elif defined(_WIN32)
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
#endif
}

inline KernelBypassSocket::~KernelBypassSocket() {
#ifdef SIMCORE_HAS_IO_URING
    io_uring_queue_exit(&ring_);
#elif defined(_WIN32)
    if (iocp_) CloseHandle(iocp_);
#endif
}

inline bool KernelBypassSocket::send(int fd, const std::string& data) {
#ifdef SIMCORE_HAS_IO_URING
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return false;
    io_uring_prep_send(sqe, fd, data.data(), data.size(), 0);
    io_uring_submit(&ring_);
    return true;
#elif defined(_WIN32)
    WSABUF buf{(ULONG)data.size(), const_cast<char*>(data.data())};
    DWORD sent = 0; OVERLAPPED ov{};
    return WSASend(fd, &buf, 1, &sent, 0, &ov, nullptr) == 0;
#else
    return ::send(fd, data.data(), data.size(), 0) == (ssize_t)data.size();
#endif
}

inline void KernelBypassSocket::poll() {
#ifdef SIMCORE_HAS_IO_URING
    io_uring_cqe* cqe = nullptr;
    while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
        io_uring_cqe_seen(&ring_, cqe);
    }
#elif defined(_WIN32)
    DWORD bytes; ULONG_PTR key; LPOVERLAPPED ov;
    while (GetQueuedCompletionStatus(iocp_, &bytes, &key, &ov, 0)) {}
#else
    // no-op fallback
#endif
}

