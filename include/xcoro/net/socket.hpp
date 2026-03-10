#pragma once

#include <asm-generic/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <stdexcept>
#include <system_error>

#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/buffer.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
namespace xcoro::net {

class socket {
 public:
  socket() noexcept = default;
  socket(io_context& ctx, int fd) noexcept : ctx_(&ctx), fd_(fd) {}

  static socket open_tcp(io_context& ctx, int family = AF_INET);
  static socket open_udp(io_context& ctx, int family = AF_INET);

  socket(socket&& other) noexcept : ctx_(other.ctx_), fd_(other.fd_) {
    other.ctx_ = nullptr;
    other.fd_ = -1;
  }
  socket& operator=(socket&& other) noexcept {
    if (this != &other) {
      ctx_ = other.ctx_;
      fd_ = other.fd_;
      other.ctx_ = nullptr;
      other.fd_ = -1;
    }
    return *this;
  }

  socket(const socket&) = delete;
  socket& operator=(const socket&) = delete;
  ~socket() { close(); }

  void bind(const endpoint& ep);
  void listen(int backlog = SOMAXCONN);
  void set_nonblocking(bool on = true);
  void set_reuse_addr(bool on = true);
  void set_reuse_port(bool on = true);
  void set_tcp_nodelay(bool on = true);

  task<> async_connect(const endpoint& ep, cancellation_token token = {});
  task<size_t> async_read_some(mutable_buffer dst, cancellation_token token = {});
  task<size_t> async_read_exact(mutable_buffer dst, cancellation_token token = {});
  task<size_t> async_write_some(const_buffer src, cancellation_token token = {});
  task<size_t> async_write_all(const_buffer src, cancellation_token token = {});

  endpoint local_endpoint() const;
  endpoint peer_endpoint() const;

  void shutdown(int how = SHUT_RDWR) {
    // 关闭接收和发送，但不立即关闭文件描述符，fd依然有效
    if (::shutdown(fd_, how) == -1) {
      throw std::system_error(errno, std::system_category(), "shutdown failed");
    }
  }

  int native_handle() const noexcept { return fd_; }

  bool is_open() const noexcept {
    return fd_ != -1;
  }

  void close() noexcept {
    if (fd_ != -1) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  void ensure_context() const {
    if (!ctx_) {
      throw std::runtime_error("socket is not bound to io_context");
    }
  }

  io_context* ctx_{nullptr};
  int fd_{-1};
};

inline socket socket::open_tcp(io_context& ctx, int family) {
  int fd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (fd == -1) {
    throw std::system_error(errno, std::system_category(), "socket(TCP) failed");
  }
  return socket{ctx, fd};
}
inline socket socket::open_udp(io_context& ctx, int family) {
  int fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == -1) {
    throw std::system_error(errno, std::system_category(), "socket(UDP) failed");
  }
  return socket{ctx, fd};
}

inline void socket::bind(const endpoint& ep) {
  if (::bind(fd_, ep.data(), ep.size()) == -1) {
    throw std::system_error(errno, std::system_category(), "bind failed");
  }
}

inline void socket::listen(int backlog) {
  if (::listen(fd_, backlog) == -1) {
    throw std::system_error(errno, std::system_category(), "listen failed");
  }
}

inline void socket::set_nonblocking(bool on) {
  int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags == -1) {
    throw std::system_error(errno, std::system_category(), "fcntl(F_GETFL) failed");
  }
  flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(fd_, F_SETFL, flags) == -1) {
    throw std::system_error(errno, std::system_category(), "fcntl(F_SETFL) failed");
  }
}

inline void socket::set_reuse_addr(bool on) {
  int v = on ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == -1) {
    throw std::system_error(errno, std::system_category(), "setsockopt(SO_REUSEADDR) failed");
  }
}

inline void socket::set_reuse_port(bool on) {
  int v = on ? 1 : 0;
  if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &v, sizeof(v)) == -1) {
    throw std::system_error(errno, std::system_category(), "setsockopt(SO_REUSEPORT) failed");
  }
}

inline void socket::set_tcp_nodelay(bool on) {
  int v = on ? 1 : 0;
  if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == -1) {
    throw std::system_error(errno, std::system_category(), "setsockopt(TCP_NODELAY) failed");
  }
}

inline task<> socket::async_connect(const endpoint& ep, cancellation_token token) {
  ensure_context();
  for (;;) {
    throw_if_cancellation_requested(token);
    int r = ::connect(fd_, ep.data(), ep.size());
    if (r == 0) {
      co_return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EINPROGRESS || errno == EALREADY) {
      // 等待 fd 可写并校验 SO_ERROR
      co_await ctx_->async_connection(fd_, token);
      co_return;
    }
    throw std::system_error(errno, std::system_category(), "connect failed");
  }
}

inline task<size_t> socket::async_read_exact(mutable_buffer dst, cancellation_token token) {
  ensure_context();
  if (dst.bytes.empty()) {
    co_return 0;
  }
  co_return co_await ctx_->async_read_exact(fd_, dst.bytes.data(), dst.bytes.size(), token);
}

inline task<std::size_t> socket::async_write_some(const_buffer src, cancellation_token token) {
  ensure_context();
  if (src.bytes.empty()) {
    co_return 0;
  }

  for (;;) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::write(fd_, src.bytes.data(), src.bytes.size());
    if (n > 0) {
      co_return static_cast<std::size_t>(n);
    }
    if (n == 0) {
      co_return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // 当前阶段复用 async_connection 的可写等待语义
      co_await ctx_->async_connection(fd_, token);
      continue;
    }
    throw std::system_error(errno, std::system_category(), "write failed");
  }
}

inline task<std::size_t> socket::async_write_all(const_buffer src, cancellation_token token) {
  ensure_context();
  if (src.bytes.empty()) {
    co_return 0;
  }
  co_return co_await ctx_->async_write_all(fd_, src.bytes.data(), src.bytes.size(), token);
}

inline endpoint socket::local_endpoint() const {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &len) == -1) {
    throw std::system_error(errno, std::system_category(), "getsockname failed");
  }
  return endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss), len);
}

inline endpoint socket::peer_endpoint() const {
  sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &len) == -1) {
    throw std::system_error(errno, std::system_category(), "getpeername failed");
  }
  return endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss), len);
}

}  // namespace xcoro::net