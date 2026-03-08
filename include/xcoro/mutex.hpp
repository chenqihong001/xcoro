#pragma once

#include "xcoroutine/cancellation.hpp"
#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

namespace xcoro {

class condition_variable;

class mutex {
public:
  mutex() = default;
  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  bool try_lock() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!locked_) {
      locked_ = true;
      return true;
    }
    return false;
  }

  void unlock() noexcept {
    std::coroutine_handle<> next;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        auto waiter = waiters_.front();
        waiters_.pop();
        if (!waiter || !waiter->try_mark_resumed()) {
          continue;
        }
        next = waiter->handle;
        break;
      }
      if (!next) {
        locked_ = false;
        return;
      }
    }
    if (next) {
      next.resume();
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
    awaiter(mutex &m, cancellation_token token = {}) noexcept
        : mutex_(m), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return mutex_.try_lock();
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      state_ = std::make_shared<waiter_state>();
      state_->handle = handle;
      if (!mutex_.enqueue_waiter(state_)) {
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
    mutex &mutex_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto lock() noexcept { return awaiter{*this}; }
  auto lock(cancellation_token token) noexcept { return awaiter{*this, std::move(token)}; }

private:
  bool enqueue_waiter(const std::shared_ptr<waiter_state> &state) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!locked_) {
      locked_ = true;
      return false;
    }
    waiters_.push(state);
    return true;
  }

  void lock_and_resume(std::coroutine_handle<> handle) noexcept {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (!locked_) {
        locked_ = true;
      } else {
        auto state = std::make_shared<waiter_state>();
        state->handle = handle;
        waiters_.push(std::move(state));
        return;
      }
    }
    if (handle) {
      handle.resume();
    }
  }

  std::mutex mutex_;
  bool locked_{false};
  std::queue<std::shared_ptr<waiter_state>> waiters_;

  friend class condition_variable;
};

} // namespace xcoro
