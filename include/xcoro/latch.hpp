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

class latch {
public:
  explicit latch(std::ptrdiff_t count) : count_(count) {}

  latch(const latch &) = delete;
  latch &operator=(const latch &) = delete;

  void count_down(std::ptrdiff_t n = 1) {
    if (n <= 0) {
      return;
    }
    std::vector<std::coroutine_handle<>> to_resume;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (count_ == 0) {
        return;
      }
      if (n >= count_) {
        count_ = 0;
        while (!waiters_.empty()) {
          auto waiter = waiters_.front();
          waiters_.pop();
          if (!waiter || !waiter->try_mark_resumed()) {
            continue;
          }
          to_resume.push_back(waiter->handle);
        }
      } else {
        count_ -= n;
      }
    }
    for (auto h : to_resume) {
      if (h) {
        h.resume();
      }
    }
  }

  bool try_wait() const noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    return count_ == 0;
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
    awaiter(latch &l, cancellation_token token = {}) noexcept
        : latch_(l), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return latch_.try_wait();
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      state_ = std::make_shared<waiter_state>();
      state_->handle = handle;
      if (!latch_.enqueue_waiter(state_)) {
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
    latch &latch_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto wait() noexcept { return awaiter{*this}; }
  auto wait(cancellation_token token) noexcept { return awaiter{*this, std::move(token)}; }

private:
  bool enqueue_waiter(const std::shared_ptr<waiter_state> &state) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (count_ == 0) {
      return false;
    }
    waiters_.push(state);
    return true;
  }

  mutable std::mutex mutex_;
  std::ptrdiff_t count_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

} // namespace xcoro
