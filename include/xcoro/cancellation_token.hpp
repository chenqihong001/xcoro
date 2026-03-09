#pragma once

#include <coroutine>
#include <memory>
#include <utility>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_state.hpp"

namespace xcoro {

class cancellation_source;
class cancellation_registration;

class cancellation_token {
 public:
  cancellation_token() noexcept = default;
  bool can_be_cancelled() const noexcept { return static_cast<bool>(state_); }
  bool is_cancellation_requested() const noexcept { return state_ && state_->is_cancellation_requested(); }

  class awaiter;
  auto operator co_await() const noexcept;

 private:
  explicit cancellation_token(std::shared_ptr<detail::cancellation_state> state) noexcept
      : state_(std::move(state)) {}

  std::shared_ptr<detail::cancellation_state> state_;
  friend class cancellation_registration;
  friend class cancellation_source;
};

class operation_cancelled : public std::exception {
 public:
  const char* what() const noexcept override { return "operation cancelled"; }
};

inline void throw_if_cancellation_requested(const cancellation_token& token) {
  if (token.is_cancellation_requested()) {
    throw operation_cancelled{};
  }
}

class cancellation_token::awaiter {
 public:
  explicit awaiter(cancellation_token token) : token_(std::move(token)) {}

  bool await_ready() const noexcept {
    // 如果该token没有正确关联state或者关联的state已经request cancellation了，那么就直接恢复协程，不挂起
    return !token_.can_be_cancelled() || token_.is_cancellation_requested();
  }

  bool await_suspend(std::coroutine_handle<> awaiting) noexcept {
    awaiting_ = awaiting;  // 保存待恢复协程
    registration_ = cancellation_registration(token_, [this]() noexcept { awaiting_.resume(); });
    return true;  // 挂起协程，将来会在注册的回调函数中恢复
  }

  void await_resume() noexcept {}

 private:
  std::coroutine_handle<> awaiting_;
  cancellation_token token_;
  cancellation_registration registration_;  // 该awaiter对象的销毁，会导致registration_的析构，cancellation_registration对象析构，会自动从绑定的state对象中的回调数组中退出
};

inline auto cancellation_token::operator co_await() const noexcept {
  return awaiter{*this};
}

template <typename Callback>
inline void cancellation_registration::register_callback(const cancellation_token& token, Callback&& callback) {
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
    // try_add_callback失败，表示已经request cancellation了，所以直接调用回调
    cb_state->invoke();
  }
}

}  // namespace xcoro