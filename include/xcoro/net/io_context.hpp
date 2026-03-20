#pragma once

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/detail/descriptor_state.hpp"
#include "xcoro/net/detail/epoll_reactor.hpp"
#include "xcoro/net/detail/io_context_access.hpp"
#include "xcoro/net/detail/no_sigpipe.hpp"
#include "xcoro/net/detail/timer_queue.hpp"
#include "xcoro/task.hpp"

namespace xcoro::net {

class socket;
class acceptor;
class resolver;

namespace detail {

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

  explicit detached_task(std::coroutine_handle<promise_type> handle) noexcept
      : handle_(handle) {}

  detached_task(detached_task&& other) noexcept
      : handle_(std::exchange(other.handle_, {})) {}

  detached_task& operator=(detached_task&& other) noexcept {
    if (this != &other) {
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  ~detached_task() = default;

 private:
  std::coroutine_handle<promise_type> handle_{};
};

}  // namespace detail

class io_context {
 public:
  io_context() : reactor_(*this) {}
  ~io_context() { stop(); }

  io_context(const io_context&) = delete;
  io_context& operator=(const io_context&) = delete;
  io_context(io_context&&) = delete;
  io_context& operator=(io_context&&) = delete;

  void run() {
    if (loop_thread_.joinable()) {
      return;
    }

    stopped_.store(false, std::memory_order_release);
    loop_thread_ = std::jthread([this](std::stop_token) { event_loop(); });
  }

  void run_in_current_thread() {
    if (loop_thread_.joinable()) {
      return;
    }

    stopped_.store(false, std::memory_order_release);
    event_loop();
  }

  void stop() {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    // 唤醒epoll
    wake();

    if (loop_thread_.joinable()) {
      loop_thread_.join();  // 等待event_loop退出
    }
  }

  // 把当前协程重新投递到io_context的ready queue，内部通过schedule_awaiter完成
  // - 把当前协程句柄入队
  // - 通过wake()唤醒事件循环
  // - 稍后由drain_ready() 恢复
  // 后续逻辑延后到下一轮调度执行
  task<> schedule() { co_await schedule_awaiter{this}; }

  task<size_t> async_read_some(int fd, void* buffer, size_t count) {
    co_return co_await async_read_some(fd, buffer, count, cancellation_token{});
  }

  // 1. 先直接::read
  // 2. EINTR 重试
  // 3. EAGAIN/EWOULDBLOCK时构造一个临时descriptor_state
  // 4. co_await wait_readable(state,token)
  // 5. 协程恢复后继续重试::read
  task<size_t> async_read_some(int fd, void* buffer, size_t count,
                               cancellation_token token) {
    if (count == 0) {
      co_return 0;
    }

    detail::descriptor_state state;
    state.ctx = this;
    state.fd = fd;
    auto* out = static_cast<std::byte*>(buffer);

    for (;;) {
      throw_if_cancellation_requested(token);
      const ssize_t n = ::read(fd, out, count);
      if (n > 0) {
        co_return static_cast<size_t>(n);
      }
      if (n == 0) {
        co_return 0;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await wait_readable(state, token);
        // 等待可读
        continue;
      }
      throw std::system_error(errno, std::system_category(), "read failed");
    }
  }

  task<size_t> async_read_exact(int fd, void* buffer, size_t count) {
    co_return co_await async_read_exact(fd, buffer, count, cancellation_token{});
  }

  // 持续读，直到读满count或遇到EOF
  task<size_t> async_read_exact(int fd, void* buffer, size_t count,
                                cancellation_token token) {
    if (count == 0) {
      co_return 0;
    }

    detail::descriptor_state state;
    state.ctx = this;
    state.fd = fd;
    auto* out = static_cast<std::byte*>(buffer);
    size_t total = 0;

    while (total < count) {
      throw_if_cancellation_requested(token);
      const ssize_t n = ::read(fd, out + total, count - total);
      if (n > 0) {
        total += static_cast<size_t>(n);
        continue;
      }
      if (n == 0) {
        co_return total;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await wait_readable(state, token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "read failed");
    }

    co_return total;
  }

  task<size_t> async_write_all(int fd, const void* buffer, size_t count) {
    co_return co_await async_write_all(fd, buffer, count, cancellation_token{});
  }

  // 持续写，直到全部写完
  // 1. 先调用detail::write_no_sigpipe
  // 2. EINTR 重试
  // 3. EAGAIN/EWOULDBLOCK时等待可写
  // 4. 继续写剩余部分
  task<size_t> async_write_all(int fd, const void* buffer, size_t count,
                               cancellation_token token) {
    if (count == 0) {
      co_return 0;
    }

    detail::descriptor_state state;
    state.ctx = this;
    state.fd = fd;
    auto* out = static_cast<const std::byte*>(buffer);
    size_t written = 0;

    while (written < count) {
      throw_if_cancellation_requested(token);
      const ssize_t n =
          detail::write_no_sigpipe(fd, out + written, count - written);
      if (n > 0) {
        written += static_cast<size_t>(n);
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await wait_writable(state, token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "write failed");
    }

    co_return written;
  }

  task<> async_accept(int fd, cancellation_token token = {}) {
    detail::descriptor_state state;
    state.ctx = this;
    state.fd = fd;
    co_await wait_readable(state, token);
  }

  task<> async_connection(int fd, cancellation_token token = {}) {
    detail::descriptor_state state;
    state.ctx = this;
    state.fd = fd;
    co_await wait_writable(state, token);

    int error = 0;
    socklen_t len = sizeof(error);
    // 检查异步连接结果
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1) {
      throw std::system_error(errno, std::system_category(),
                              "getsockopt(SO_ERROR) failed");
    }
    if (error != 0) {
      throw std::system_error(error, std::system_category(), "connect failed");
    }
  }

  // 把一个task<T>以detached方式丢进io_context里执行，不关心返回值结果
  template <typename T>
  void spawn(task<T> task_value) {
    auto starter = [this](task<T> inner) -> detail::detached_task {
      co_await schedule();
      co_await std::move(inner);
    };
    (void)starter(std::move(task_value));
  }

  // 创建定时等待
  template <typename Rep, typename Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    co_await timer_awaiter{this, deadline, cancellation_token{}};
  }

  // 计算deadline
  // 构造timer_awaiter
  // 把timer state 放入timer_queue
  template <typename Rep, typename Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> duration,
                   cancellation_token token) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    co_await timer_awaiter{this, deadline, token};
  }

 private:
  friend class socket;
  friend class acceptor;
  friend class resolver;
  friend struct detail::io_context_access;

  // 把协程挂到ready queue
  class schedule_awaiter {
   public:
    explicit schedule_awaiter(io_context* ctx) noexcept : ctx_(ctx) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) noexcept {
      ctx_->enqueue_ready(handle);
      ctx_->wake();
    }

    void await_resume() const noexcept {}

   private:
    io_context* ctx_ = nullptr;
  };

  // 代表一次“等待fd可读/可写”的挂起操作
  class fd_wait_awaiter {
   public:
    fd_wait_awaiter(io_context* ctx, detail::descriptor_state* state,
                    detail::wait_kind kind, cancellation_token token) noexcept
        : ctx_(ctx), state_(state), kind_(kind), token_(std::move(token)) {}

    // 如果协程已经不再等待，但operation_还未完成，则通知reactor取消这次等待
    ~fd_wait_awaiter() {
      if (ctx_ != nullptr && state_ != nullptr &&
          !operation_.completed.load(std::memory_order_acquire)) {
        ctx_->reactor_.cancel_wait(*state_, kind_, operation_);
      }
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        // cancel触发，则标记操作完成，return false不挂起
        operation_.reset(handle);
        operation_.cancelled.store(true, std::memory_order_release);
        operation_.completed.store(true, std::memory_order_release);
        return false;  // 如果token已取消，则直接走inline取消路径
      }

      return ctx_->reactor_.arm_wait(*state_, kind_, operation_, handle, token_);
    }

    void await_resume() {
      // 检查cancelled和error
      if (operation_.cancelled.load(std::memory_order_acquire)) {
        throw operation_cancelled{};
      }

      const int error = operation_.error.load(std::memory_order_acquire);
      if (error != 0) {
        throw std::system_error(error, std::system_category(),
                                "descriptor wait failed");
      }
    }

   private:
    io_context* ctx_ = nullptr;
    // 当前等待对应的descriptor共享状态
    detail::descriptor_state* state_ = nullptr;        // 记录这个fd当前有哪些等待者
    detail::wait_kind kind_{detail::wait_kind::read};  // 等待方向，可读/可写
    cancellation_token token_;
    detail::wait_operation_state operation_;  // 记录这一次等待自己的完成状态
  };

  // 表示一次等到某个deadline的挂起操作
  class timer_awaiter {
   public:
    timer_awaiter(io_context* ctx,
                  std::chrono::steady_clock::time_point deadline,
                  cancellation_token token) noexcept
        : ctx_(ctx), deadline_(deadline), token_(std::move(token)) {}

    ~timer_awaiter() {
      if (state_ != nullptr) {
        state_->completed.store(true, std::memory_order_release);
      }
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
      state_ = std::make_shared<detail::timer_queue::timer_state>();
      state_->ctx = ctx_;
      state_->handle = handle;

      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        state_->cancelled.store(true, std::memory_order_release);
        state_->completed.store(true, std::memory_order_release);
        return false;
      }

      // 注册取消，入timer queue，并唤醒事件循环
      if (token_.can_be_cancelled()) {
        registration_ =
            cancellation_registration(token_, [state = state_, ctx = ctx_]() noexcept {
              if (!state->completed.exchange(true, std::memory_order_acq_rel)) {
                state->cancelled.store(true, std::memory_order_release);
                detail::io_context_access::enqueue_ready(*ctx, state->handle);
                detail::io_context_access::wake(*ctx);
              }
            });
      }

      ctx_->timers_.push(deadline_, state_);
      ctx_->wake();
      return true;
    }

    void await_resume() {
      // 如果被取消，则抛operation_canncelled异常
      if (state_ != nullptr &&
          state_->cancelled.load(std::memory_order_acquire)) {
        throw operation_cancelled{};
      }
    }

   private:
    io_context* ctx_ = nullptr;
    std::chrono::steady_clock::time_point deadline_{};
    cancellation_token token_;
    cancellation_registration registration_{};
    std::shared_ptr<detail::timer_queue::timer_state> state_;
  };

  // 构造fd_wait_awaiter 等待可读事件
  task<> wait_readable(detail::descriptor_state& state,
                       cancellation_token token = {}) {
    co_await fd_wait_awaiter{this, &state, detail::wait_kind::read,
                             std::move(token)};
  }

  // 构造fd_wait_awaiter 等待可写事件
  task<> wait_writable(detail::descriptor_state& state,
                       cancellation_token token = {}) {
    co_await fd_wait_awaiter{this, &state, detail::wait_kind::write,
                             std::move(token)};
  }

  // 把某个fd从reactor中注销，并恢复仍然挂在这个fd上的等待者
  void unregister_descriptor(detail::descriptor_state& state,
                             int error = EBADF) noexcept {
    // EBADF Bad File Descriptor无效的文件描述符
    reactor_.unregister_descriptor(state, error);
  }

  // 把待恢复协程放入ready_队列
  void enqueue_ready(std::coroutine_handle<> handle) noexcept {
    std::lock_guard lock(ready_mutex_);
    ready_.push(handle);
  }

  // 唤醒epoll_wait()
  void wake() noexcept { reactor_.wake(); }

  // 把ready_队列整体取出，然后逐个resume()
  void drain_ready() {
    std::queue<std::coroutine_handle<>> queue;
    {  // 最小化锁区域
      std::lock_guard lock(ready_mutex_);
      std::swap(queue, ready_);
    }

    while (!queue.empty()) {
      auto handle = queue.front();
      queue.pop();
      if (handle) {
        handle.resume();
      }
    }
  }

  // 从timers_收集已到期的timer，并把对应协程入ready queue
  void resume_due_timers() {
    std::vector<std::shared_ptr<detail::timer_queue::timer_state>> due;
    timers_.collect_due(due);

    for (auto& state : due) {
      if (state != nullptr &&
          !state->completed.exchange(true, std::memory_order_acq_rel)) {
        enqueue_ready(state->handle);
      }
    }
  }

  // 主事件循环
  void event_loop() {
    while (!stopped_.load(std::memory_order_acquire)) {
      reactor_.poll_once(timers_.timeout_ms());
      resume_due_timers();
      drain_ready();
    }

    resume_due_timers();
    drain_ready();
    // 退出前，完成所有ready工作
  }

  detail::timer_queue timers_;     // 保存所有定时器
  detail::epoll_reactor reactor_;  // 负责和epoll/eventfd交互

  std::mutex ready_mutex_;                     // 保护ready_队列
  std::queue<std::coroutine_handle<>> ready_;  // 保存即将被恢复的协程句柄

  std::jthread loop_thread_;         // 后台事件循环线程，run()时启动
  std::atomic_bool stopped_{false};  // 标志事件循环是否停止
};

}  // namespace xcoro::net

namespace xcoro::net::detail {

inline void io_context_access::enqueue_ready(io_context& ctx,
                                             std::coroutine_handle<> handle) noexcept {
  ctx.enqueue_ready(handle);
}

inline void io_context_access::wake(io_context& ctx) noexcept { ctx.wake(); }

}  // namespace xcoro::net::detail
