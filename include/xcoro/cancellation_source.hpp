#pragma once

#include <memory>

#include "xcoro/cancellation_state.hpp"
#include "xcoro/cancellation_token.hpp"

namespace xcoro {

class cancellation_source {
 public:
  cancellation_source() : state_(std::make_shared<detail::cancellation_state>()) {}

  cancellation_token token() const noexcept {
    return cancellation_token(state_);
  }
  bool request_cancellation() noexcept {
    return state_ ? state_->request_cancellation() : false;
  }

  bool is_cancellation_requested() const noexcept {
    return state_ && state_->is_cancellation_requested();
  }

 private:
  std::shared_ptr<detail::cancellation_state> state_;
};

}  // namespace xcoro