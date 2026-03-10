#pragma once
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
#include "xcoro/net/socket.hpp"
namespace xcoro::net {

class acceptor {
 public:
  acceptor() = delete;
  acceptor(io_context& ctx, socket listen_sock)
      : ctx_(&ctx), sock_(std::move(listen_sock)) {}

  static acceptor bind(io_context& ctx, const endpoint& ep, int backlog = SOMAXCONN) {
    auto s = socket::open_tcp(ctx, ep.family());
    s.set_reuse_addr(true);
    s.set_reuse_port(true);
    s.set_nonblocking(true);
    s.bind(ep);
    s.listen(backlog);
    return acceptor{ctx, std::move(s)};
  }

  socket& native_socket() noexcept { return sock_; }
  const socket& native_socket() const noexcept { return sock_; }

  task<socket> async_accept(cancellation_token token = {}) {
    for (;;) {
      throw_if_cancellation_requested(token);

      sockaddr_storage ss{};
      socklen_t len = sizeof(ss);
      int fd = ::accept4(sock_.native_handle(),
                         reinterpret_cast<sockaddr*>(&ss),
                         &len,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd >= 0) {
        co_return socket{ctx(), fd};
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await ctx().async_accept(sock_.native_handle(), token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "accept failed");
    }
  }

 private:
  io_context& ctx() {
    if (!ctx_) {
      throw std::runtime_error("acceptor has no io_context");
    }
    return *ctx_;
  }

  io_context* ctx_{nullptr};
  socket sock_;
};

}  // namespace xcoro::net