#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

namespace xcoro::net {

class io_context;

}  // namespace xcoro::net

namespace xcoro::net::detail {

class timer_queue {
 public:
  struct timer_state {
    io_context* ctx = nullptr;
    std::coroutine_handle<> handle{};
    std::atomic_bool completed{false};
    std::atomic_bool cancelled{false};
  };

  void push(std::chrono::steady_clock::time_point deadline,
            std::shared_ptr<timer_state> state) {
    std::lock_guard lock(mutex_);
    heap_.push(timer_item{deadline, std::move(state)});
  }

  int timeout_ms() const noexcept {
    std::lock_guard lock(mutex_);
    if (heap_.empty()) {
      return -1;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto deadline = heap_.top().deadline;
    if (deadline <= now) {
      return 0;
    }

    const auto diff =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(diff.count());
  }

  void collect_due(std::vector<std::shared_ptr<timer_state>>& out) {
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(mutex_);
    while (!heap_.empty() && heap_.top().deadline <= now) {
      out.push_back(std::move(heap_.top().state));
      heap_.pop();
    }
  }

 private:
  struct timer_item {
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<timer_state> state;
  };

  struct timer_cmp {
    bool operator()(const timer_item& lhs, const timer_item& rhs) const noexcept {
      return lhs.deadline > rhs.deadline;
    }
  };

  mutable std::mutex mutex_;
  std::priority_queue<timer_item, std::vector<timer_item>, timer_cmp> heap_;
};

}  // namespace xcoro::net::detail
