#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "xcoro/net/detail/operation_state.hpp"

namespace xcoro::net {

class io_context;

}  // namespace xcoro::net

namespace xcoro::net::detail {

enum class wait_kind {
  read,
  write,
};

struct waiter_slot {
  wait_operation_state* operation = nullptr;
};

struct descriptor_state {
  io_context* ctx = nullptr;
  int fd = -1;

  mutable std::mutex mutex;
  uint32_t registered_events = 0;
  bool registered_with_epoll = false;
  bool closing = false;

  waiter_slot read_waiter;
  waiter_slot write_waiter;
};

inline std::shared_ptr<descriptor_state> make_descriptor_state(io_context& ctx, int fd) {
  auto state = std::make_shared<descriptor_state>();
  state->ctx = &ctx;
  state->fd = fd;
  return state;
}

}  // namespace xcoro::net::detail
