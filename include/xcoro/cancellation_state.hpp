#pragma once
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
namespace xcoro {

class cancellation_token;

namespace detail {
class cancellation_state;

class callback_state {
 public:
  explicit callback_state(std::function<void()>&& cb) : callback_(std::move(cb)) {}
  void invoke() noexcept {
    if (!invoked_.exchange(true, std::memory_order_acq_rel)) {
      try {
        callback_();
      } catch (...) {
        std::terminate();
      }
    }
  }

 private:
  std::function<void()> callback_;
  std::atomic<bool> invoked_{false};
};

class cancellation_state : public std::enable_shared_from_this<cancellation_state> {
 public:
  bool is_cancellation_requested() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }
  bool try_add_callback(const std::shared_ptr<callback_state>& cb) {
    if (is_cancellation_requested()) {
      return false;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    if (cancelled_.load(std::memory_order_relaxed)) {
      return false;
    }
    callbacks_.push_back(cb);
    return true;
  }

  void remove_callback(const std::shared_ptr<callback_state>& cb) noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
      if (*it == cb) {
        callbacks_.erase(it);
        break;
      }
    }
  }

  bool request_cancellation() noexcept {
    bool expected = false;
    if (!cancelled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return false;
    }
    std::vector<std::shared_ptr<callback_state>> callbacks;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      callbacks.swap(callbacks_);  // 把callbacks_置空
    }
    // 把回调列表在持锁期间“搬出去”，然后释放锁后再逐个调用
    // - 保证回调只执行一次：callbacks_被清空（swap后为空），后续不会再被执行
    // - 异常/noexcept友好：swap是noexcept，不会在request_cancellation()里抛异常
    // - 降低临界区事件：只在swap时持锁，减少锁竞争
    for (auto& cb : callbacks) {
      cb->invoke();
    }
    return true;
  }

 private:
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  std::vector<std::shared_ptr<callback_state>> callbacks_;
};

}  // namespace detail

}  // namespace xcoro