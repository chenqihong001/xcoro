#pragma once
#include <coroutine>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

namespace xcoro {

template <typename T>
class generator;

namespace detail {

template <typename T>
class generator_promise {
 public:
  using value_type = std::remove_reference_t<T>;
  using reference_type = std::conditional_t<std::is_reference_v<T>, T, T&>;
  using pointer_type = value_type*;
  generator_promise() = default;

  generator<T> get_return_object() noexcept;

  std::suspend_always initial_suspend() const { return {}; }

  std::suspend_always final_suspend() const noexcept { return {}; }

  template <typename U = T>
    requires(!std::is_rvalue_reference_v<U>)
  auto yield_value(std::remove_reference_t<U>& value) noexcept {
    value_ = std::addressof(value);
    return std::suspend_always{};
  }

  auto yield_value(std::remove_reference_t<T>&& value) noexcept {
    value_ = std::addressof(value);
    return std::suspend_always{};
  }
  // co_yield右值会被提升到coroutine frame中
  // co_yield 10; 编译器会生成类似的逻辑 int temp =10; promise.yield_value(temp); suspend(); 在协程中可以保存右值地址

  void unhandled_exception() {
    exception_ = std::current_exception();
  }

  void return_void() noexcept {}

  reference_type value() const noexcept { return static_cast<reference_type>(*value_); }

  void rethrow_if_exception() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

 private:
  pointer_type value_ = nullptr;
  std::exception_ptr exception_;
};

struct generator_sentinel {
};

template <typename T>
class generator_iterator {
  using coroutine_handle = std::coroutine_handle<generator_promise<T>>;

 public:
  using iterator_category = std::input_iterator_tag;
  using difference_type = std::ptrdiff_t;
  using value_type = typename generator_promise<T>::value_type;
  using reference = typename generator_promise<T>::reference_type;
  using pointer = typename generator_promise<T>::pointer_type;
  generator_iterator() noexcept = default;

  explicit generator_iterator(coroutine_handle coroutine) noexcept : coroutine_(coroutine) {}

  friend bool operator==(const generator_iterator& it, generator_sentinel) noexcept {
    return it.coroutine_ == nullptr || it.coroutine_.done();
  }

  friend bool operator!=(const generator_iterator& it, generator_sentinel s) noexcept {
    return !(it == s);
  }

  friend bool operator==(generator_iterator s, const generator_iterator& it) noexcept {
    return it == s;
  }
  friend bool operator!=(generator_iterator s, const generator_iterator& it) noexcept {
    return !(it == s);
  }

  generator_iterator& operator++() {
    coroutine_.resume();
    if (coroutine_.done()) {
      coroutine_.promise().rethrow_if_exception();
    }
    return *this;
  }

  reference operator*() const noexcept { return coroutine_.promise().value(); }
  pointer operator->() const noexcept { return std::addressof(operator*()); }

 private:
  coroutine_handle coroutine_{nullptr};
};

}  // namespace detail

template <typename T>
class [[nodiscard]] generator {
 public:
  using promise_type = detail::generator_promise<T>;
  using iterator = detail::generator_iterator<T>;
  using sentinel = detail::generator_sentinel;

  generator() noexcept : coroutine_(nullptr) {}
  generator(const generator&) = delete;
  generator(generator&& other) noexcept : coroutine_(other.coroutine_) { other.coroutine_ = nullptr; }

  generator& operator=(const generator&) = delete;
  generator& operator=(generator&& other) {
    if (std::addressof(other) != this) {
      if (coroutine_) {
        coroutine_.destroy();
      }
      coroutine_ = std::exchange(other.coroutine_, nullptr);
    }
    return *this;
  }

  ~generator() {
    if (coroutine_) {
      coroutine_.destroy();
    }
  }

  iterator begin() {
    if (coroutine_ != nullptr) {
      coroutine_.resume();
      if (coroutine_.done()) {
        coroutine_.promise().rethrow_if_exception();
      }
    }
    return iterator{coroutine_};
  }

  sentinel end() noexcept {
    return sentinel{};
  }

 private:
  friend class detail::generator_promise<T>;
  explicit generator(std::coroutine_handle<promise_type> coroutine) noexcept : coroutine_(coroutine) {}
  std::coroutine_handle<promise_type> coroutine_;
};

namespace detail {

template <typename T>
generator<T> generator_promise<T>::get_return_object() noexcept {
  return generator<T>(std::coroutine_handle<generator_promise<T>>::from_promise(*this));
}
}  // namespace detail

}  // namespace xcoro