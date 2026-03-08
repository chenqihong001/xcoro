#pragma once

#include "xcoroutine/cancellation.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace xcoro {

class semaphore {
public:
  explicit semaphore(std::ptrdiff_t initial_count = 0) : count_(initial_count) {}

  semaphore(const semaphore &) = delete;
  semaphore &operator=(const semaphore &) = delete;

  bool try_acquire() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (count_ > 0) {
      --count_;
      return true;
    }
    return false;
  }

  void release(std::ptrdiff_t n = 1) {
    if (n <= 0) {
      return;
    }
    std::vector<std::coroutine_handle<>> to_resume;
    to_resume.reserve(static_cast<size_t>(n));

    {
      std::lock_guard<std::mutex> guard(mutex_);
      for (std::ptrdiff_t i = 0; i < n; ++i) {
        bool resumed = false;
        while (!waiters_.empty()) {
          auto waiter = waiters_.front();
          waiters_.pop();
          if (!waiter || !waiter->try_mark_resumed()) {
            continue;
          }
          to_resume.push_back(waiter->handle);
          resumed = true;
          break;
        }
        if (!resumed) {
          ++count_;
        }
      }
    }

    for (auto h : to_resume) {
      if (h) {
        h.resume();
      }
    }
  }

private:
  struct waiter_state {
    std::coroutine_handle<> handle{};
    std::atomic<bool> resumed{false};
    std::atomic<bool> cancelled{false};

    bool try_mark_resumed() noexcept {
      return !resumed.exchange(true, std::memory_order_acq_rel);
    }
  };

public:
  struct awaiter {
    awaiter(semaphore &sem, cancellation_token token = {}) noexcept
        : sem_(sem), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return sem_.try_acquire();
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      state_ = std::make_shared<waiter_state>();
      state_->handle = handle;
      if (!sem_.enqueue_waiter(state_)) {
        state_.reset();
        return false;
      }

      if (token_.can_be_cancelled()) {
        registration_ = cancellation_registration(
            token_, [state = state_]() noexcept {
              if (!state->try_mark_resumed()) {
                return;
              }
              state->cancelled.store(true, std::memory_order_release);
              if (state->handle) {
                state->handle.resume();
              }
            });
      }

      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        if (state_->try_mark_resumed()) {
          state_->cancelled.store(true, std::memory_order_release);
        }
        return false;
      }

      return true;
    }

    void await_resume() {
      if (cancelled_immediate_ ||
          (state_ && state_->cancelled.load(std::memory_order_acquire))) {
        throw operation_cancelled{};
      }
    }

  private:
    semaphore &sem_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto acquire() noexcept { return awaiter{*this}; }
  auto acquire(cancellation_token token) noexcept { return awaiter{*this, std::move(token)}; }

private:
  bool enqueue_waiter(const std::shared_ptr<waiter_state> &state) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (count_ > 0) {
      --count_;
      return false;
    }
    waiters_.push(state);
    return true;
  }

  std::mutex mutex_;
  std::ptrdiff_t count_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

} // namespace xcoro
