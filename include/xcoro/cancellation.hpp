#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace xcoro {

class cancellation_token;

namespace detail {

class cancellation_state;

class callback_state {
public:
  explicit callback_state(std::function<void()> cb) : callback_(std::move(cb)) {}

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

  bool try_add_callback(const std::shared_ptr<callback_state> &cb) {
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

  void remove_callback(const std::shared_ptr<callback_state> &cb) noexcept {
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
      callbacks.swap(callbacks_);
    }

    for (auto &cb : callbacks) {
      cb->invoke();
    }
    return true;
  }

private:
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  std::vector<std::shared_ptr<callback_state>> callbacks_;
};

} // namespace detail

class cancellation_registration {
public:
  cancellation_registration() noexcept = default;

  template <typename Callback>
  cancellation_registration(const cancellation_token &token, Callback &&callback) {
    register_callback(token, std::forward<Callback>(callback));
  }

  cancellation_registration(const cancellation_registration &) = delete;
  cancellation_registration &operator=(const cancellation_registration &) = delete;

  cancellation_registration(cancellation_registration &&other) noexcept
      : state_(std::move(other.state_)), callback_(std::move(other.callback_)) {}

  cancellation_registration &operator=(cancellation_registration &&other) noexcept {
    if (this != &other) {
      deregister();
      state_ = std::move(other.state_);
      callback_ = std::move(other.callback_);
    }
    return *this;
  }

  ~cancellation_registration() { deregister(); }

  bool is_registered() const noexcept { return static_cast<bool>(state_); }

  void deregister() noexcept {
    if (state_) {
      state_->remove_callback(callback_);
      state_.reset();
      callback_.reset();
    }
  }

private:
  template <typename Callback>
  void register_callback(const cancellation_token &token, Callback &&callback);

  std::shared_ptr<detail::cancellation_state> state_;
  std::shared_ptr<detail::callback_state> callback_;
};

class cancellation_token {
public:
  cancellation_token() noexcept = default;

  bool can_be_cancelled() const noexcept { return static_cast<bool>(state_); }

  bool is_cancellation_requested() const noexcept {
    return state_ && state_->is_cancellation_requested();
  }

  class awaiter;

  auto operator co_await() const noexcept;

private:
  explicit cancellation_token(std::shared_ptr<detail::cancellation_state> state) noexcept
      : state_(std::move(state)) {}

  std::shared_ptr<detail::cancellation_state> state_;
  friend class cancellation_source;
  friend class cancellation_registration;
};

class cancellation_token::awaiter {
public:
  explicit awaiter(cancellation_token token) : token_(std::move(token)) {}

  bool await_ready() const noexcept {
    return !token_.can_be_cancelled() || token_.is_cancellation_requested();
  }

  bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
    awaiting_ = awaiting;
    registration_ = cancellation_registration(
        token_, [this]() noexcept { awaiting_.resume(); });
    return true;
  }

  void await_resume() noexcept {}

private:
  cancellation_token token_;
  cancellation_registration registration_;
  std::coroutine_handle<> awaiting_{};
};

inline auto cancellation_token::operator co_await() const noexcept {
  return awaiter{*this};
}

class cancellation_source {
public:
  cancellation_source() : state_(std::make_shared<detail::cancellation_state>()) {}

  cancellation_token token() const noexcept { return cancellation_token{state_}; }

  bool request_cancellation() noexcept {
    return state_ ? state_->request_cancellation() : false;
  }

  bool is_cancellation_requested() const noexcept {
    return state_ && state_->is_cancellation_requested();
  }

private:
  std::shared_ptr<detail::cancellation_state> state_;
};

class operation_cancelled : public std::exception {
public:
  const char *what() const noexcept override { return "operation cancelled"; }
};

inline void throw_if_cancellation_requested(const cancellation_token &token) {
  if (token.is_cancellation_requested()) {
    throw operation_cancelled{};
  }
}

template <typename Callback>
void cancellation_registration::register_callback(const cancellation_token &token,
                                                  Callback &&callback) {
  if (!token.can_be_cancelled()) {
    return;
  }
  auto state = token.state_;
  auto cb_state = std::make_shared<detail::callback_state>(
      std::function<void()>(std::forward<Callback>(callback)));
  if (state->try_add_callback(cb_state)) {
    state_ = std::move(state);
    callback_ = std::move(cb_state);
  } else {
    cb_state->invoke();
  }
}

} // namespace xcoro
