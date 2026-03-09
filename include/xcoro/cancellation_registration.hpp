#pragma once

#include <memory>
#include <utility>

#include "xcoro/cancellation_state.hpp"

namespace xcoro {

class cancellation_token;

class cancellation_registration {
 public:
  cancellation_registration() noexcept = default;
  template <typename Callback>
  cancellation_registration(const cancellation_token& token, Callback&& callback) {
    register_callback(token, callback);
  }

  cancellation_registration(const cancellation_registration&) = delete;
  cancellation_registration& operator=(const cancellation_registration&) = delete;

  cancellation_registration(cancellation_registration&& other) noexcept
      : state_(std::move(other.state_)),
        callback_(std::move(other.callback_)) {}

  cancellation_registration& operator=(cancellation_registration&& other) noexcept {
    if (this != &other) {
      deregister();
      state_ = std::move(other.state_);
      callback_ = std::move(other.callback_);
    }
    return *this;
  }

  void deregister() noexcept {
    if (state_) {
      state_->remove_callback(callback_);
      state_.reset();
      callback_.reset();
    }
  }

  ~cancellation_registration() {
    deregister();
  }

 private:
  template <typename Callback>
  void register_callback(const cancellation_token& token, Callback&& callback);

  std::shared_ptr<detail::cancellation_state> state_;
  std::shared_ptr<detail::callback_state> callback_;
};

}  // namespace xcoro