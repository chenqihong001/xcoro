#pragma once
#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace xcoro {

class condition_variable {
 public:
  condition_variable() = default;
  condition_variable(const condition_variable&) = delete;
  condition_variable& operator=(const condition_variable&) = delete;

 private:
  struct waiter_state {
    std::coroutine_handle<> handle{};
    mutex* mutex{};
    std::atomic<bool> resumed{false};
    std::atomic<bool> cancelled{false};

    bool try_mark_resumed() noexcept {
      return !resumed.exchange(true, std::memory_order_acq_rel);
    }
  };

 public:
  struct awaiter {
    awaiter(condition_variable& cv, mutex& m, cancellation_token token = {}) noexcept
        : cv_(cv), mutex_(m), token_(std::move(token)) {}

    bool await_ready() noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return true;
      }
      return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_immediate_ = true;
        return false;
      }

      state_ = std::make_shared<waiter_state>();
      state_->handle = handle;
      state_->mutex = &mutex_;
      cv_.enqueue_waiter(state_);
      mutex_.unlock();

      if (token_.can_be_cancelled()) {
        registration_ = cancellation_registration(
            token_, [state = state_]() noexcept {
              if (!state->try_mark_resumed()) {
                return;
              }
              state->cancelled.store(true, std::memory_order_release);
              if (state->mutex) {
                state->mutex->lock_and_resume(state->handle);
              }
            });
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
    condition_variable& cv_;
    mutex& mutex_;
    cancellation_token token_;
    cancellation_registration registration_;
    std::shared_ptr<waiter_state> state_;
    bool cancelled_immediate_{false};
  };

  auto wait(mutex& m) noexcept { return awaiter{*this, m}; }
  auto wait(mutex& m, cancellation_token token) noexcept {
    return awaiter{*this, m, std::move(token)};
  }

  void notify_one() {
    std::shared_ptr<waiter_state> w;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        w = waiters_.front();
        waiters_.pop();
        if (!w || !w->try_mark_resumed()) {
          w.reset();
          continue;
        }
        break;
      }
    }
    if (w && w->mutex) {
      w->mutex->lock_and_resume(w->handle);
    }
  }

  void notify_all() {
    std::vector<std::shared_ptr<waiter_state>> to_resume;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      while (!waiters_.empty()) {
        auto w = waiters_.front();
        waiters_.pop();
        if (!w || !w->try_mark_resumed()) {
          continue;
        }
        to_resume.push_back(std::move(w));
      }
    }
    for (auto& w : to_resume) {
      if (w && w->mutex) {
        w->mutex->lock_and_resume(w->handle);
      }
    }
  }

 private:
  void enqueue_waiter(const std::shared_ptr<waiter_state>& w) {
    std::lock_guard<std::mutex> guard(mutex_);
    waiters_.push(w);
  }

  std::mutex mutex_;
  std::queue<std::shared_ptr<waiter_state>> waiters_;
};

}  // namespace xcoro
