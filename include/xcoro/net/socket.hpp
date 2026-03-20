#pragma once

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <system_error>

#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/buffer.hpp"
#include "xcoro/net/detail/descriptor_state.hpp"
#include "xcoro/net/detail/fd_ops.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"

namespace xcoro::net {

class socket {
 public:
  socket() noexcept = default;
  socket(io_context& ctx, int fd) noexcept
      : state_(detail::make_descriptor_state(ctx, fd)) {}
  explicit socket(std::shared_ptr<detail::descriptor_state> state) noexcept
      : state_(std::move(state)) {}

  static socket open_tcp(io_context& ctx, int family = AF_INET) {
    try {
      return socket{detail::make_descriptor_state(
          ctx, detail::create_socket(family, SOCK_STREAM, IPPROTO_TCP))};
    } catch (const std::system_error& error) {
      throw std::system_error(error.code(), "socket(TCP) failed");
    }
  }

  static socket open_udp(io_context& ctx, int family = AF_INET) {
    try {
      return socket{detail::make_descriptor_state(
          ctx, detail::create_socket(family, SOCK_DGRAM, IPPROTO_UDP))};
    } catch (const std::system_error& error) {
      throw std::system_error(error.code(), "socket(UDP) failed");
    }
  }

  socket(socket&&) noexcept = default;
  socket& operator=(socket&& other) noexcept {
    if (this != &other) {
      close();
      state_ = std::move(other.state_);
    }
    return *this;
  }

  socket(const socket&) = delete;
  socket& operator=(const socket&) = delete;

  ~socket() { close(); }

  bool is_open() const noexcept {
    return state_ != nullptr && state_->fd != -1;
  }

  int native_handle() const noexcept { return is_open() ? state_->fd : -1; }

  void bind(const endpoint& ep) {
    ensure_open();
    if (::bind(native_handle(), ep.data(), ep.size()) == -1) {
      throw std::system_error(errno, std::system_category(), "bind failed");
    }
  }

  void listen(int backlog = SOMAXCONN) {
    ensure_open();
    if (::listen(native_handle(), backlog) == -1) {
      throw std::system_error(errno, std::system_category(), "listen failed");
    }
  }

  void shutdown(int how = SHUT_RDWR) {
    ensure_open();
    if (::shutdown(native_handle(), how) == -1) {
      throw std::system_error(errno, std::system_category(), "shutdown failed");
    }
  }

  void close() noexcept {
    if (!is_open()) {
      return;
    }

    if (state_->ctx != nullptr) {
      state_->ctx->unregister_descriptor(*state_, EBADF);
    }
    ::close(state_->fd);
    state_->fd = -1;
  }

  void set_nonblocking(bool on = true) {
    ensure_open();
    detail::set_nonblocking(native_handle(), on);
  }

  void set_reuse_address(bool on = true) {
    ensure_open();
    detail::set_socket_option(native_handle(), SOL_SOCKET, SO_REUSEADDR,
                              on ? 1 : 0);
  }

  void set_reuse_addr(bool on = true) { set_reuse_address(on); }

  void set_reuse_port(bool on = true) {
    ensure_open();
    detail::set_socket_option(native_handle(), SOL_SOCKET, SO_REUSEPORT,
                              on ? 1 : 0);
  }

  void set_tcp_nodelay(bool on = true) {
    ensure_open();
    detail::set_socket_option(native_handle(), IPPROTO_TCP, TCP_NODELAY,
                              on ? 1 : 0);
  }

  task<> async_connect(const endpoint& ep, cancellation_token token = {}) {
    ensure_open();

    for (;;) {
      throw_if_cancellation_requested(token);

      const int rc = ::connect(native_handle(), ep.data(), ep.size());
      if (rc == 0 || errno == EISCONN) {
        co_return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EINPROGRESS || errno == EALREADY) {
        co_await context().wait_writable(descriptor(), token);
        check_connect_result();
        co_return;
      }
      throw std::system_error(errno, std::system_category(), "connect failed");
    }
  }

  task<size_t> async_read_some(mutable_buffer dst, cancellation_token token = {}) {
    ensure_open();
    if (dst.bytes.empty()) {
      co_return 0;
    }

    for (;;) {
      throw_if_cancellation_requested(token);
      // 先直接调用::read如果读到数据，直接返回
      const ssize_t n =
          ::read(native_handle(), dst.bytes.data(), dst.bytes.size());
      if (n > 0) {
        co_return static_cast<size_t>(n);
      }
      // 连接正常关闭，没有更多数据可读
      if (n == 0) {
        co_return 0;
      }
      // 进程被信号中断
      if (errno == EINTR) {
        continue;
      }
      // 当前没有数据可读
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await context().wait_readable(descriptor(), token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "read failed");
    }
  }

  task<size_t> async_read_exact(mutable_buffer dst, cancellation_token token = {}) {
    ensure_open();
    if (dst.bytes.empty()) {
      co_return 0;
    }

    size_t total = 0;
    while (total < dst.bytes.size()) {
      throw_if_cancellation_requested(token);
      const ssize_t n =
          ::read(native_handle(), dst.bytes.data() + total,
                 dst.bytes.size() - total);
      if (n > 0) {
        total += static_cast<size_t>(n);
        continue;
      }
      if (n == 0) {
        co_return total;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await context().wait_readable(descriptor(), token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "read failed");
    }

    co_return total;
  }

  task<size_t> async_write_some(const_buffer src, cancellation_token token = {}) {
    ensure_open();
    if (src.bytes.empty()) {
      co_return 0;
    }

    for (;;) {
      throw_if_cancellation_requested(token);
      const ssize_t n = ::write(native_handle(), src.bytes.data(),
                                src.bytes.size());
      if (n > 0) {
        co_return static_cast<size_t>(n);
      }
      if (n == 0) {
        co_return 0;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await context().wait_writable(descriptor(), token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "write failed");
    }
  }

  task<size_t> async_write_all(const_buffer src, cancellation_token token = {}) {
    ensure_open();
    if (src.bytes.empty()) {
      co_return 0;
    }

    size_t written = 0;
    while (written < src.bytes.size()) {
      throw_if_cancellation_requested(token);
      const ssize_t n =
          ::write(native_handle(), src.bytes.data() + written,
                  src.bytes.size() - written);
      if (n > 0) {
        written += static_cast<size_t>(n);
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await context().wait_writable(descriptor(), token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "write failed");
    }

    co_return written;
  }

  endpoint local_endpoint() const {
    ensure_open();
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(native_handle(), reinterpret_cast<sockaddr*>(&storage),
                      &length) == -1) {
      throw std::system_error(errno, std::system_category(),
                              "getsockname failed");
    }
    return endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&storage),
                                   length);
  }

  endpoint peer_endpoint() const {
    ensure_open();
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getpeername(native_handle(), reinterpret_cast<sockaddr*>(&storage),
                      &length) == -1) {
      throw std::system_error(errno, std::system_category(),
                              "getpeername failed");
    }
    return endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&storage),
                                   length);
  }

 private:
  friend class acceptor;

  io_context& context() const {
    if (state_ == nullptr || state_->ctx == nullptr) {
      throw std::runtime_error("socket is not bound to io_context");
    }
    return *state_->ctx;
  }

  detail::descriptor_state& descriptor() const {
    if (state_ == nullptr) {
      throw std::runtime_error("socket has no descriptor state");
    }
    return *state_;
  }

  void ensure_open() const {
    if (!is_open()) {
      throw std::runtime_error("socket is not open");
    }
  }

  void check_connect_result() const {
    const int error = detail::get_socket_error(native_handle());
    if (error != 0) {
      throw std::system_error(error, std::system_category(), "connect failed");
    }
  }

  std::shared_ptr<detail::descriptor_state> state_;
};

}  // namespace xcoro::net
