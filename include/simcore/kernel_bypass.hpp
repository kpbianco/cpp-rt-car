#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <string>
#include <utility>

#if defined(__linux__)
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#if defined(__linux__) && __has_include(<liburing.h>) && defined(SIMCORE_ENABLE_IO_URING)
#include <liburing.h>
#define SIMCORE_KERNEL_BYPASS_HAS_IO_URING 1
#endif

namespace simcore {

// Lightweight helper that wraps optional kernel-bypass primitives. On Linux we
// support io_uring when SIMCORE_ENABLE_IO_URING is defined at build time. When
// the optimized path is unavailable the helper transparently falls back to
// blocking send() semantics while still honouring non-blocking descriptors.
class KernelBypassSocket {
 public:
  explicit KernelBypassSocket(unsigned queue_depth = 64)
      : depth_(queue_depth) {
#ifdef SIMCORE_KERNEL_BYPASS_HAS_IO_URING
    if (io_uring_queue_init(depth_, &ring_, 0) == 0) {
      available_ = true;
    } else {
      depth_ = 0;
    }
#endif
  }

  ~KernelBypassSocket() { drain();
#ifdef SIMCORE_KERNEL_BYPASS_HAS_IO_URING
    if (available_) {
      io_uring_queue_exit(&ring_);
    }
#endif
  }

  KernelBypassSocket(const KernelBypassSocket&) = delete;
  KernelBypassSocket& operator=(const KernelBypassSocket&) = delete;

  KernelBypassSocket(KernelBypassSocket&&) = delete;
  KernelBypassSocket& operator=(KernelBypassSocket&&) = delete;

  bool available() const { return available_; }

  unsigned depth() const { return depth_; }

  // Submit a payload for transmission. The payload is only consumed (moved
  // from) when the submission succeeds. Returns false if the queue is full or
  // the descriptor would block.
  bool submit(int fd, std::string& payload, int flags = default_send_flags()) {
#ifdef SIMCORE_KERNEL_BYPASS_HAS_IO_URING
    if (available_) {
      if (pending_.size() >= depth_) {
        return false;
      }
      poll();
      io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
      if (!sqe) {
        return false;
      }
      pending_.emplace_back();
      auto it = std::prev(pending_.end());
      it->fd = fd;
      it->data = std::move(payload);
      it->self = it;
      io_uring_prep_send(sqe, fd, it->data.data(), it->data.size(), flags);
      io_uring_sqe_set_data(sqe, &(*it));
      int ret = io_uring_submit(&ring_);
      if (ret < 0) {
        payload = std::move(it->data);
        pending_.erase(it);
        return false;
      }
      return true;
    }
#endif
    return blocking_send(fd, payload, flags);
  }

  // Reap completed submissions without blocking.
  void poll() {
#ifdef SIMCORE_KERNEL_BYPASS_HAS_IO_URING
    if (!available_) {
      return;
    }
    io_uring_cqe* cqe = nullptr;
    while (io_uring_peek_cqe(&ring_, &cqe) == 0 && cqe) {
      if (auto* pending = static_cast<Pending*>(io_uring_cqe_get_data(cqe))) {
        pending_.erase(pending->self);
      }
      io_uring_cqe_seen(&ring_, cqe);
    }
#endif
  }

  // Block until all outstanding submissions are completed.
  void drain() {
#ifdef SIMCORE_KERNEL_BYPASS_HAS_IO_URING
    if (!available_) {
      return;
    }
    io_uring_cqe* cqe = nullptr;
    while (!pending_.empty()) {
      int ret = io_uring_wait_cqe(&ring_, &cqe);
      if (ret < 0) {
        break;
      }
      if (cqe) {
        if (auto* pending = static_cast<Pending*>(io_uring_cqe_get_data(cqe))) {
          pending_.erase(pending->self);
        }
        io_uring_cqe_seen(&ring_, cqe);
      }
    }
#endif
  }

  static int default_send_flags() {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
  }

 private:
#ifdef SIMCORE_KERNEL_BYPASS_HAS_IO_URING
  struct Pending {
    int fd = -1;
    std::string data;
    std::list<Pending>::iterator self;
  };

#endif

  static bool blocking_send(int fd, const std::string& payload, int flags) {
#if defined(_WIN32)
    SOCKET socket = static_cast<SOCKET>(fd);
    const char* data = payload.data();
    std::size_t remaining = payload.size();
    while (remaining > 0) {
      int chunk = static_cast<int>(std::min<std::size_t>(
          remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
      int sent = ::send(socket, data, chunk, flags);
      if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEINTR || err == WSAEWOULDBLOCK)
          return false;
        return false;
      }
      data += sent;
      remaining -= static_cast<std::size_t>(sent);
    }
    return true;
#else
    const char* data = payload.data();
    std::size_t remaining = payload.size();
    while (remaining > 0) {
      ssize_t sent = ::send(fd, data, remaining, flags);
      if (sent < 0) {
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return false;
        return false;
      }
      data += sent;
      remaining -= static_cast<std::size_t>(sent);
    }
    return true;
#endif
  }

#ifdef SIMCORE_KERNEL_BYPASS_HAS_IO_URING
  io_uring ring_{};
  bool available_ = false;
  unsigned depth_ = 0;
  std::list<Pending> pending_{};
#else
  bool available_ = false;
  unsigned depth_ = 0;
#endif
};

}  // namespace simcore

