#pragma once

#include <stdexcept>
#include <utility>

#include "xcoroutine/net/address.hpp"
#include "xcoroutine/net/io_scheduler.hpp"
#include "xcoroutine/net/socket.hpp"
#include "xcoroutine/net/tcp_stream.hpp"

namespace xcoro::net {

class tcp_acceptor {
 public:
  tcp_acceptor() = delete;
  tcp_acceptor(io_scheduler& scheduler, socket sock)
      : scheduler_(&scheduler), sock_(std::move(sock)) {}

  tcp_acceptor(const tcp_acceptor&) = delete;
  tcp_acceptor& operator=(const tcp_acceptor&) = delete;
  tcp_acceptor(tcp_acceptor&&) noexcept = default;
  tcp_acceptor& operator=(tcp_acceptor&&) noexcept = default;

  static tcp_acceptor listen(io_scheduler& scheduler,
                             const address& addr,
                             int backlog = SOMAXCONN) {
    const auto ver = addr.version();
    if (ver == ip_version::unknown) {
      throw std::runtime_error("unsupported address version");
    }
    socket sock = socket::create_tcp(ver);
    sock.set_reuse_addr(true);
    sock.set_reuse_port(true);
    sock.set_nonblocking(true);
    sock.bind(addr);
    sock.listen(backlog);
    return tcp_acceptor{scheduler, std::move(sock)};
  }

  socket& native_socket() noexcept { return sock_; }
  const socket& native_socket() const noexcept { return sock_; }

  task<tcp_stream> accept() {
    auto [client, peer] = co_await scheduler().async_accept(sock_);
    (void)peer;
    co_return tcp_stream{scheduler(), std::move(client)};
  }

  task<tcp_stream> accept(cancellation_token token) {
    auto [client, peer] = co_await scheduler().async_accept(sock_, token);
    (void)peer;
    co_return tcp_stream{scheduler(), std::move(client)};
  }

 private:
  io_scheduler& scheduler() {
    if (!scheduler_) {
      throw std::runtime_error("tcp_acceptor has no scheduler bound");
    }
    return *scheduler_;
  }

  io_scheduler* scheduler_{nullptr};
  socket sock_;
};

}  // namespace xcoro::net
