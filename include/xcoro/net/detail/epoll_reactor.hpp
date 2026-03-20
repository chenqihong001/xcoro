#pragma once

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <coroutine>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/detail/descriptor_state.hpp"
#include "xcoro/net/detail/io_context_access.hpp"

namespace xcoro::net {

class io_context;

}  // namespace xcoro::net

namespace xcoro::net::detail {

class epoll_reactor {
 public:
  explicit epoll_reactor(io_context& ctx) : ctx_(&ctx) {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
      throw std::system_error(errno, std::system_category(),
                              "epoll_create1 failed");
    }

    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);  // 初始计数器为0，设置非阻塞模式，设置close-on-exec
    // eventfd创建一个特殊的内核对象，包含一个64位计数器，可用于进程间通信或者线程间同步，作为事件通知机制
    // 写入：通过write()或send()向这个fd写入一个8字节整数，会增加计数器的值
    // 读取：通过read()或recv()从这个id读取，会返回计数器的当前值，并将计数器重置为0
    // EFD_CLOEXEC 防止文件描述符在exec()执行新程序时被意外继承
    if (wake_fd_ == -1) {
      const int saved_errno = errno;
      ::close(epoll_fd_);
      throw std::system_error(saved_errno, std::system_category(),
                              "eventfd failed");
    }

    epoll_event event{};
    event.events = EPOLLIN;  // 注册eventfd可读事件
    event.data.u64 = kWakeTag;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) == -1) {
      const int saved_errno = errno;
      ::close(wake_fd_);  // ::close本身也可能失败并修改errno
      ::close(epoll_fd_);
      throw std::system_error(saved_errno, std::system_category(),
                              "epoll_ctl add wake_fd failed");
    }
  }

  ~epoll_reactor() {
    if (wake_fd_ != -1) {
      ::close(wake_fd_);
    }
    if (epoll_fd_ != -1) {
      ::close(epoll_fd_);
    }
  }

  epoll_reactor(const epoll_reactor&) = delete;
  epoll_reactor& operator=(const epoll_reactor&) = delete;

  void wake() noexcept {
    if (wake_fd_ == -1) {
      return;
    }

    const uint64_t one = 1;
    ::write(wake_fd_, &one, sizeof(one));
  }

  // 将一个等待操作装配到系统中，准备在条件满足时触发
  bool arm_wait(descriptor_state& state, wait_kind kind, wait_operation_state& operation,
                std::coroutine_handle<> handle,
                cancellation_token token) {
    std::lock_guard lock(state.mutex);

    if (state.fd == -1 || state.closing) {
      operation.reset(handle);
      operation.error.store(EBADF, std::memory_order_release);
      operation.completed.store(true, std::memory_order_release);
      return false;
    }

    auto& slot = slot_for(state, kind);
    if (slot.operation != nullptr) {
      throw std::logic_error(
          "concurrent waits on the same descriptor direction are not allowed");
    }

    operation.reset(handle);
    slot.operation = &operation;
    update_interest_locked(state);

    if (token.can_be_cancelled()) {
      operation.registration =
          cancellation_registration(token, [this, &state, kind, &operation]() noexcept {
            this->cancel_wait(state, kind, operation);
          });
    }

    return true;
  }

  void cancel_wait(descriptor_state& state, wait_kind kind,
                   wait_operation_state& operation) noexcept {
    std::coroutine_handle<> handle;
    {
      std::lock_guard lock(state.mutex);
      auto& slot = slot_for(state, kind);
      if (slot.operation != &operation) {
        return;
      }

      slot.operation = nullptr;
      update_interest_locked(state);
      complete_operation_locked(operation, true, 0, handle);
    }

    if (handle) {
      io_context_access::enqueue_ready(*ctx_, handle);
      io_context_access::wake(*ctx_);
    }
  }

  void unregister_descriptor(descriptor_state& state,
                             int error = EBADF) noexcept {
    std::vector<std::coroutine_handle<>> ready;
    {
      std::lock_guard lock(state.mutex);
      state.closing = true;

      if (state.registered_with_epoll && state.fd != -1) {
        (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, state.fd, nullptr);
      }

      state.registered_with_epoll = false;
      state.registered_events = 0;

      if (state.read_waiter.operation != nullptr) {
        auto* operation = std::exchange(state.read_waiter.operation, nullptr);
        std::coroutine_handle<> handle;
        complete_operation_locked(*operation, false, error, handle);
        if (handle) {
          ready.push_back(handle);
        }
      }

      if (state.write_waiter.operation != nullptr) {
        auto* operation = std::exchange(state.write_waiter.operation, nullptr);
        std::coroutine_handle<> handle;
        complete_operation_locked(*operation, false, error, handle);
        if (handle) {
          ready.push_back(handle);
        }
      }
    }

    for (auto handle : ready) {
      io_context_access::enqueue_ready(*ctx_, handle);
    }
    if (!ready.empty()) {
      io_context_access::wake(*ctx_);
    }
  }

  void poll_once(int timeout_ms) {
    constexpr int kMaxEvents = 64;
    std::array<epoll_event, kMaxEvents> events{};
    const int count =
        ::epoll_wait(epoll_fd_, events.data(), kMaxEvents, timeout_ms);
    if (count < 0) {
      if (errno == EINTR) {
        return;
      }
      throw std::system_error(errno, std::system_category(), "epoll_wait failed");
    }

    for (int i = 0; i < count; ++i) {
      if (events[i].data.u64 == kWakeTag) {
        drain_wake_fd();
        continue;
      }

      auto* state =
          static_cast<descriptor_state*>(events[i].data.ptr);
      if (state != nullptr) {
        dispatch_descriptor_event(*state, events[i].events);
      }
    }
  }

 private:
  static waiter_slot& slot_for(descriptor_state& state, wait_kind kind) noexcept {
    return kind == wait_kind::read ? state.read_waiter : state.write_waiter;
  }

  static void complete_operation_locked(wait_operation_state& operation,
                                        bool cancelled, int error,
                                        std::coroutine_handle<>& ready) noexcept {
    operation.registration = {};
    operation.cancelled.store(cancelled, std::memory_order_release);
    operation.error.store(error, std::memory_order_release);

    if (!operation.completed.exchange(true, std::memory_order_acq_rel)) {
      ready = operation.handle;
    }
  }

  void update_interest_locked(descriptor_state& state) {
    uint32_t desired = 0;
    if (state.read_waiter.operation != nullptr) {
      desired |= EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    }
    if (state.write_waiter.operation != nullptr) {
      desired |= EPOLLOUT | EPOLLERR | EPOLLHUP;
    }

    if (desired == 0) {
      if (state.registered_with_epoll && state.fd != -1) {
        (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, state.fd, nullptr);
      }
      state.registered_with_epoll = false;
      state.registered_events = 0;
      return;
    }

    if (state.registered_with_epoll && state.registered_events == desired) {
      return;
    }

    epoll_event event{};
    event.events = desired;
    event.data.ptr = &state;

    const int op = state.registered_with_epoll ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(epoll_fd_, op, state.fd, &event) == -1) {
      if (errno == ENOENT && op == EPOLL_CTL_MOD) {
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, state.fd, &event) == -1) {
          throw std::system_error(errno, std::system_category(),
                                  "epoll_ctl add failed");
        }
      } else if (errno == EEXIST && op == EPOLL_CTL_ADD) {
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, state.fd, &event) == -1) {
          throw std::system_error(errno, std::system_category(),
                                  "epoll_ctl mod failed");
        }
      } else {
        throw std::system_error(errno, std::system_category(), "epoll_ctl failed");
      }
    }

    state.registered_with_epoll = true;
    state.registered_events = desired;
  }

  void dispatch_descriptor_event(descriptor_state& state, uint32_t events) noexcept {
    std::vector<std::coroutine_handle<>> ready;
    {
      std::lock_guard lock(state.mutex);
      if (state.closing) {
        return;
      }

      const bool read_ready =
          (events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
      const bool write_ready =
          (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0;

      if (read_ready && state.read_waiter.operation != nullptr) {
        auto* operation = std::exchange(state.read_waiter.operation, nullptr);
        std::coroutine_handle<> handle;
        complete_operation_locked(*operation, false, 0, handle);
        if (handle) {
          ready.push_back(handle);
        }
      }

      if (write_ready && state.write_waiter.operation != nullptr) {
        auto* operation = std::exchange(state.write_waiter.operation, nullptr);
        std::coroutine_handle<> handle;
        complete_operation_locked(*operation, false, 0, handle);
        if (handle) {
          ready.push_back(handle);
        }
      }

      try {
        update_interest_locked(state);
      } catch (...) {
        // 事件分发路径不向外抛异常，避免中断事件循环。
      }
    }

    for (auto handle : ready) {
      io_context_access::enqueue_ready(*ctx_, handle);
    }
  }

  void drain_wake_fd() noexcept {
    uint64_t value = 0;
    while (::read(wake_fd_, &value, sizeof(value)) > 0) {
    }
  }

  static constexpr uint64_t kWakeTag = 0xffffffffffffffffULL;

  io_context* ctx_ = nullptr;
  int epoll_fd_ = -1;
  int wake_fd_ = -1;
};

}  // namespace xcoro::net::detail
