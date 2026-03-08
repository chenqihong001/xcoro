#pragma once
#include "awaitable_traits.hpp"
#include "cancellation.hpp"
#include "event.hpp"
#include "xcoroutine/concepts/awaitable.hpp"
#include "xcoroutine/task.hpp"
#include "xcoroutine/utils/void_value.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
namespace xcoro {

namespace detail {

template <typename R>
struct when_any_storage {
  using value_type = std::conditional_t<std::is_reference_v<R>,
                                        std::add_pointer_t<std::remove_reference_t<R>>,
                                        std::remove_const_t<R>>;

  value_type value{};

  static when_any_storage from(R &&v) {
    if constexpr (std::is_reference_v<R>) {
      return when_any_storage{std::addressof(v)};
    } else {
      return when_any_storage{std::forward<R>(v)};
    }
  }

  decltype(auto) get() & {
    if constexpr (std::is_reference_v<R>) {
      return static_cast<R>(*value);
    } else {
      return (value);
    }
  }

  decltype(auto) get() && {
    if constexpr (std::is_reference_v<R>) {
      return static_cast<R>(*value);
    } else {
      return std::move(value);
    }
  }
};

template <>
struct when_any_storage<void> {
  using value_type = void_value;
  value_type value{};

  static when_any_storage from() { return {}; }

  void get() & noexcept {}
  void get() && noexcept {}
};

struct detached_task {
  struct promise_type {
    detached_task get_return_object() noexcept {
      return detached_task{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { std::terminate(); }
  };

  explicit detached_task(std::coroutine_handle<promise_type>) noexcept {}
  ~detached_task() = default;
};

/**
 * @brief 管理多个任务的完成状态，确保只有第一个完成的任务被记录
 */
template <typename RetType>
class first_completion_tracker {
public:
  first_completion_tracker() = default;

  bool try_become_first(std::size_t index) noexcept {
    bool expected = false;
    if (first_completed_.compare_exchange_strong(expected, true)) {
      index_ = index;
      return true;
    }
    return false;
  }

  void set_result(RetType &&result) { result_.emplace(std::move(result)); }

  void set_exception(std::exception_ptr ex) { exception_ = std::move(ex); }

  RetType take_result() { return std::move(result_.value()); }

  bool has_result() const noexcept { return result_.has_value(); }

  bool has_exception() const noexcept { return static_cast<bool>(exception_); }

  std::exception_ptr exception() const noexcept { return exception_; }

  std::size_t index() const noexcept { return index_; }

private:
  std::atomic<bool> first_completed_{false};
  std::optional<RetType> result_;
  std::exception_ptr exception_;
  std::size_t index_{static_cast<std::size_t>(-1)};
};

template <typename Result, std::size_t Index, concepts::Awaitable Awaitable>
detached_task when_any_task(Awaitable &&awaitable,
                            first_completion_tracker<Result> &tracker,
                            event &notify_event) {
  try {
    using await_result_t = awaiter_result_t<Awaitable>;
    if constexpr (std::is_void_v<await_result_t>) {
      co_await std::forward<Awaitable>(awaitable);
      if (tracker.try_become_first(Index)) {
        using holder_t = when_any_storage<void>;
        Result result{Index, typename Result::variant_type{std::in_place_index<Index>,
                                                           holder_t::from()}};
        tracker.set_result(std::move(result));
        notify_event.set();
      }
    } else {
      decltype(auto) r = co_await std::forward<Awaitable>(awaitable);
      if (tracker.try_become_first(Index)) {
        using holder_t = when_any_storage<await_result_t>;
        Result result{Index,
                      typename Result::variant_type{
                          std::in_place_index<Index>,
                          holder_t::from(std::forward<decltype(r)>(r))}};
        tracker.set_result(std::move(result));
        notify_event.set();
      }
    }
  } catch (...) {
    if (tracker.try_become_first(Index)) {
      tracker.set_exception(std::current_exception());
      notify_event.set();
    }
  }
  co_return;
}

template <typename Result, concepts::Awaitable Awaitable>
detached_task when_any_task_range(Awaitable &&awaitable,
                                  first_completion_tracker<Result> &tracker,
                                  event &notify_event,
                                  std::size_t index) {
  try {
    using await_result_t = awaiter_result_t<Awaitable>;
    if constexpr (std::is_void_v<await_result_t>) {
      co_await std::forward<Awaitable>(awaitable);
      if (tracker.try_become_first(index)) {
        Result result{index, when_any_storage<void>::from()};
        tracker.set_result(std::move(result));
        notify_event.set();
      }
    } else {
      decltype(auto) r = co_await std::forward<Awaitable>(awaitable);
      if (tracker.try_become_first(index)) {
        Result result{index, when_any_storage<await_result_t>::from(std::forward<decltype(r)>(r))};
        tracker.set_result(std::move(result));
        notify_event.set();
      }
    }
  } catch (...) {
    if (tracker.try_become_first(index)) {
      tracker.set_exception(std::current_exception());
      notify_event.set();
    }
  }
  co_return;
}

} // namespace detail

template <typename... Awaitables>
struct when_any_result {
  using variant_type =
      std::variant<detail::when_any_storage<awaiter_result_t<Awaitables>>...>;

  std::size_t index{};
  variant_type result;

  template <std::size_t I> decltype(auto) get() & {
    return std::get<I>(result).get();
  }

  template <std::size_t I> decltype(auto) get() && {
    return std::get<I>(std::move(result)).get();
  }
};

template <concepts::Awaitable Awaitable>
struct when_any_range_result {
  using storage_type = detail::when_any_storage<awaiter_result_t<Awaitable>>;

  std::size_t index{};
  storage_type result;

  decltype(auto) get() & { return result.get(); }
  decltype(auto) get() && { return std::move(result).get(); }
};

template <concepts::Awaitable... Awaitables>
[[nodiscard]] task<when_any_result<Awaitables...>> when_any(Awaitables &&...awaitables) {
  static_assert(sizeof...(Awaitables) > 0, "when_any requires at least one awaitable");
  using result_t = when_any_result<Awaitables...>;
  detail::first_completion_tracker<result_t> tracker;
  event notify;

  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (detail::when_any_task<result_t, I, Awaitables>(std::forward<Awaitables>(awaitables),
                                                    tracker, notify),
     ...);
  }(std::index_sequence_for<Awaitables...>{});

  co_await notify;
  if (tracker.has_exception()) {
    std::rethrow_exception(tracker.exception());
  }
  co_return tracker.take_result();
}

template <std::ranges::range RangeType,
          concepts::Awaitable Awaitable = std::ranges::range_value_t<RangeType>>
[[nodiscard]] task<when_any_range_result<Awaitable>> when_any(RangeType awaitables) {
  using result_t = when_any_range_result<Awaitable>;
  detail::first_completion_tracker<result_t> tracker;
  event notify;

  std::size_t index = 0;
  for (auto &&aw : awaitables) {
    detail::when_any_task_range<result_t>(std::move(aw), tracker, notify, index);
    ++index;
  }

  if (index == 0) {
    throw std::runtime_error("when_any requires at least one awaitable");
  }

  co_await notify;
  if (tracker.has_exception()) {
    std::rethrow_exception(tracker.exception());
  }
  co_return tracker.take_result();
}

template <concepts::Awaitable... Awaitables>
[[nodiscard]] task<when_any_result<Awaitables...>> when_any(cancellation_source &source,
                                                            Awaitables &&...awaitables) {
  try {
    auto result = co_await when_any(std::forward<Awaitables>(awaitables)...);
    source.request_cancellation();
    co_return result;
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

template <std::ranges::range RangeType,
          concepts::Awaitable Awaitable = std::ranges::range_value_t<RangeType>>
[[nodiscard]] task<when_any_range_result<Awaitable>> when_any(cancellation_source &source,
                                                              RangeType awaitables) {
  try {
    auto result = co_await when_any(std::forward<RangeType>(awaitables));
    source.request_cancellation();
    co_return result;
  } catch (...) {
    source.request_cancellation();
    throw;
  }
}

} // namespace xcoro
