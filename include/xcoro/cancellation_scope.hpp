#pragma once
#include "xcoro/cancellation_source.hpp"
#include "xcoro/cancellation_token.hpp"
namespace xcoro {

class cancellation_scope {
 public:
  cancellation_scope() = default;
  cancellation_token token() const noexcept {
    return source_.token();
  }
  bool request_cancellation() noexcept {
    return source_.request_cancellation();
  }
  bool is_cancellation_requested() const noexcept {
    return source_.is_cancellation_requested();
  }

  cancellation_source& source() noexcept {
    return source_;
  }

 private:
  cancellation_source source_;
};

}  // namespace xcoro