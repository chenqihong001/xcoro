#pragma once

#include <atomic>
#include <coroutine>

#include "xcoro/cancellation_registration.hpp"

namespace xcoro::net::detail {

// 单次等待操作的状态。
// 它归某个 awaiter 所有，descriptor 只临时持有裸指针。
struct wait_operation_state {
  std::coroutine_handle<> handle{};
  cancellation_registration registration{};
  std::atomic_bool completed{false};
  std::atomic_bool cancelled{false};
  std::atomic_int error{0};

  void reset(std::coroutine_handle<> new_handle) noexcept {
    handle = new_handle;
    registration = {};
    completed.store(false, std::memory_order_release);
    cancelled.store(false, std::memory_order_release);
    error.store(0, std::memory_order_release);
  }
};

}  // namespace xcoro::net::detail
