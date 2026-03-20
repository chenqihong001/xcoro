#pragma once

#include <sys/socket.h>

#include <stdexcept>
#include <system_error>

#include "xcoro/net/detail/fd_ops.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
#include "xcoro/net/socket.hpp"

namespace xcoro::net {

struct listen_options {
  int backlog = SOMAXCONN;
  bool reuse_address = true;
  bool reuse_port = true;
};

class acceptor {
 public:
  acceptor() = delete;
  acceptor(io_context& ctx, socket listen_socket) noexcept
      : ctx_(&ctx), socket_(std::move(listen_socket)) {}

  static acceptor listen(io_context& ctx, const endpoint& ep,
                         listen_options options = {}) {
    auto listen_socket = socket::open_tcp(ctx, ep.family());
    if (options.reuse_address) {
      listen_socket.set_reuse_address(true);
    }
    if (options.reuse_port) {
      listen_socket.set_reuse_port(true);
    }
    listen_socket.bind(ep);
    listen_socket.listen(options.backlog);
    return acceptor{ctx, std::move(listen_socket)};
  }

  // 兼容旧接口。
  static acceptor bind(io_context& ctx, const endpoint& ep,
                       int backlog = SOMAXCONN) {
    return listen(ctx, ep, listen_options{backlog, true, true});
  }

  socket& socket_ref() noexcept { return socket_; }
  const socket& socket_ref() const noexcept { return socket_; }

  socket& native_socket() noexcept { return socket_; }
  const socket& native_socket() const noexcept { return socket_; }

  task<socket> async_accept(cancellation_token token = {}) {
    for (;;) {
      throw_if_cancellation_requested(token);

      sockaddr_storage storage{};
      socklen_t length = sizeof(storage);
      const int fd = detail::accept_nonblocking(
          socket_.native_handle(), reinterpret_cast<sockaddr*>(&storage),
          &length);
      if (fd >= 0) {
        co_return socket{ctx(), fd};
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await ctx().wait_readable(socket_.descriptor(), token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "accept failed");
    }
  }

 private:
  io_context& ctx() {
    if (ctx_ == nullptr) {
      throw std::runtime_error("acceptor has no io_context");
    }
    return *ctx_;
  }

  io_context* ctx_ = nullptr;
  socket socket_;
};

}  // namespace xcoro::net
