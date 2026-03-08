#pragma once

#include <stdexcept>
#include <utility>

#include "xcoroutine/net/buffer.hpp"
#include "xcoroutine/net/io_scheduler.hpp"
#include "xcoroutine/net/socket.hpp"

namespace xcoro::net {

class tcp_stream {
 public:
  tcp_stream() = delete;
  explicit tcp_stream(socket sock) : sock_(std::move(sock)) {}
  tcp_stream(io_scheduler& scheduler, socket sock)
      : scheduler_(&scheduler), sock_(std::move(sock)) {}

  tcp_stream(const tcp_stream&) = delete;
  tcp_stream& operator=(const tcp_stream&) = delete;
  tcp_stream(tcp_stream&&) noexcept = default;
  tcp_stream& operator=(tcp_stream&&) noexcept = default;

  socket& native_socket() noexcept { return sock_; }
  const socket& native_socket() const noexcept { return sock_; }

  void set_scheduler(io_scheduler& scheduler) noexcept { scheduler_ = &scheduler; }

  task<size_t> async_read_some(buffer& buf, size_t max_bytes = 0) {
    return scheduler().async_read_some(sock_, buf, max_bytes);
  }

  task<size_t> async_read_exact(buffer& buf, size_t count) {
    return scheduler().async_read_exact(sock_, buf, count);
  }

  task<size_t> async_write_all(buffer& buf, size_t max_bytes = 0) {
    return scheduler().async_write_all(sock_, buf, max_bytes);
  }

  task<size_t> async_read_some(void* data, size_t count) {
    return scheduler().async_read_some(sock_.fd(), data, count);
  }

  task<size_t> async_read_exact(void* data, size_t count) {
    return scheduler().async_read_exact(sock_.fd(), data, count);
  }

  task<size_t> async_write_all(const void* data, size_t count) {
    return scheduler().async_write_all(sock_.fd(), data, count);
  }

  task<size_t> async_read_some(buffer& buf, size_t max_bytes, cancellation_token token) {
    return scheduler().async_read_some(sock_, buf, max_bytes, token);
  }

  task<size_t> async_read_exact(buffer& buf, size_t count, cancellation_token token) {
    return scheduler().async_read_exact(sock_, buf, count, token);
  }

  task<size_t> async_write_all(buffer& buf, size_t max_bytes, cancellation_token token) {
    return scheduler().async_write_all(sock_, buf, max_bytes, token);
  }

  task<size_t> async_read_some(void* data, size_t count, cancellation_token token) {
    return scheduler().async_read_some(sock_.fd(), data, count, token);
  }

  task<size_t> async_read_exact(void* data, size_t count, cancellation_token token) {
    return scheduler().async_read_exact(sock_.fd(), data, count, token);
  }

  task<size_t> async_write_all(const void* data, size_t count, cancellation_token token) {
    return scheduler().async_write_all(sock_.fd(), data, count, token);
  }

 private:
  io_scheduler& scheduler() {
    if (!scheduler_) {
      throw std::runtime_error("tcp_stream has no scheduler bound");
    }
    return *scheduler_;
  }

  io_scheduler* scheduler_{nullptr};
  socket sock_;
};

}  // namespace xcoro::net
