#pragma once

#include <coroutine>

namespace xcoro::net {

class io_context;

}  // namespace xcoro::net

namespace xcoro::net::detail {

struct io_context_access {
  static void enqueue_ready(io_context& ctx, std::coroutine_handle<> handle) noexcept;
  static void wake(io_context& ctx) noexcept;
};

}  // namespace xcoro::net::detail
