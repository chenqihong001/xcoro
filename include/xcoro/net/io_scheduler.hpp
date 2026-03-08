#pragma once
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <queue>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "xcoroutine/cancellation.hpp"
#include "xcoroutine/net/address.hpp"
#include "xcoroutine/net/buffer.hpp"
#include "xcoroutine/net/socket.hpp"
#include "xcoroutine/task.hpp"

namespace xcoro::net {

enum io_event {
  READ = EPOLLIN,
  WRITE = EPOLLOUT,
  ERROR = EPOLLERR,
  HANGUP = EPOLLHUP,
  RD_HANGUUP = EPOLLRDHUP,
  ONESHOT = EPOLLONESHOT
};

class io_awaiter;
class schedule_awaiter;
class timer_awaiter;

class io_scheduler {
 public:
  io_scheduler();
  ~io_scheduler();

  io_scheduler(const io_scheduler&) = delete;
  io_scheduler& operator=(const io_scheduler&) = delete;
  io_scheduler(io_scheduler&&) = delete;
  io_scheduler& operator=(io_scheduler&&) = delete;

  void run();                    // 启动事件循环线程
  void run_in_current_thread();  // 在当前线程运行事件循环（阻塞）
  void stop();                   // 停止调度
  task<> schedule();             // 让出执行权并切换到事件循环线程

  // I/O操作（fd级）
  task<size_t> async_read_some(int fd, void* buffer, size_t count);
  task<size_t> async_read_exact(int fd, void* buffer, size_t count);
  task<size_t> async_write_all(int fd, const void* buffer, size_t count);

  task<size_t> async_read_some(int fd, void* buffer, size_t count, cancellation_token token);
  task<size_t> async_read_exact(int fd, void* buffer, size_t count, cancellation_token token);
  task<size_t> async_write_all(int fd, const void* buffer, size_t count, cancellation_token token);

  // 兼容旧命名：async_read/async_write
  task<size_t> async_read(int fd, void* buffer, size_t count);
  task<size_t> async_write(int fd, const void* buffer, size_t count);
  task<size_t> async_read(int fd, void* buffer, size_t count, cancellation_token token);
  task<size_t> async_write(int fd, const void* buffer, size_t count, cancellation_token token);

  // I/O操作（socket/buffer级）
  task<size_t> async_read_some(socket& sock, buffer& buf, size_t max_bytes = 0);
  task<size_t> async_read_exact(socket& sock, buffer& buf, size_t count);
  task<size_t> async_write_all(socket& sock, buffer& buf, size_t max_bytes = 0);

  task<size_t> async_read_some(socket& sock, buffer& buf, size_t max_bytes, cancellation_token token);
  task<size_t> async_read_exact(socket& sock, buffer& buf, size_t count, cancellation_token token);
  task<size_t> async_write_all(socket& sock, buffer& buf, size_t max_bytes, cancellation_token token);

  // 兼容旧命名：async_read/async_write
  task<size_t> async_read(socket& sock, buffer& buf, size_t max_bytes = 0);
  task<size_t> async_write(socket& sock, buffer& buf, size_t max_bytes = 0);
  task<size_t> async_read(socket& sock, buffer& buf, size_t max_bytes, cancellation_token token);
  task<size_t> async_write(socket& sock, buffer& buf, size_t max_bytes, cancellation_token token);

  task<> async_accept(int fd);  // 等待可accept
  task<std::pair<socket, std::shared_ptr<address>>> async_accept(socket& listen_sock);

  task<> async_accept(int fd, cancellation_token token);
  task<std::pair<socket, std::shared_ptr<address>>> async_accept(socket& listen_sock, cancellation_token token);

  task<> async_connection(int fd);                     // 仅等待connect完成
  task<> async_connect(socket& sock, const address&);  // 发起连接并等待完成

  task<> async_connection(int fd, cancellation_token token);
  task<> async_connect(socket& sock, const address&, cancellation_token token);

  // fire-and-forget：把任务投递到事件循环线程执行
  template <typename T>
  void spawn(task<T> t);

  // 定时器
  template <typename Rep, typename Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> duration);

  template <typename Rep, typename Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> duration, cancellation_token token);

 private:
  friend class io_awaiter;
  friend class schedule_awaiter;
  friend class timer_awaiter;

  static constexpr std::uint64_t kWakeTag = 0xffffffffffffffffULL;
  static constexpr size_t kDefaultReadSize = 4096;

  void enqueue_ready(std::coroutine_handle<> h) noexcept;
  void wake() noexcept;
  void event_loop();
  void drain_ready();
  void drain_timers();
  int calc_timeout_ms() noexcept;

  struct timer_state {
    io_scheduler* scheduler{};
    std::coroutine_handle<> handle{};
    std::atomic<bool> completed{false};
    std::atomic<bool> cancelled{false};

    void resume_once() noexcept {
      if (!completed.exchange(true, std::memory_order_acq_rel)) {
        scheduler->enqueue_ready(handle);
      }
    }
  };

  struct timer_item {
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<timer_state> state;
  };
  struct timer_cmp {
    bool operator()(const timer_item& a, const timer_item& b) const noexcept {
      return a.deadline > b.deadline;
    }
  };

  int epoll_fd_{-1};
  int wake_fd_{-1};
  std::jthread event_loop_;
  std::atomic<bool> stopped_{false};

  std::mutex ready_mu_;
  std::queue<std::coroutine_handle<>> ready_;

  std::mutex timer_mu_;
  std::priority_queue<timer_item, std::vector<timer_item>, timer_cmp> timers_;
};

class io_awaiter {
 public:
  io_awaiter(io_scheduler* scheduler, int fd, int events,
             cancellation_token token = {})
      : scheduler_(scheduler), fd_(fd), events_(events), token_(std::move(token)) {}

  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) noexcept;
  void await_resume();

  void on_event(std::uint32_t events) noexcept;

 private:
  void resume_once() noexcept;

  io_scheduler* scheduler_;
  int fd_;
  int events_;
  std::coroutine_handle<> handle_;
  int error_{0};
  std::atomic<bool> completed_{false};
  std::atomic<bool> cancelled_{false};
  cancellation_token token_;
  cancellation_registration registration_;
};

class schedule_awaiter {
 public:
  explicit schedule_awaiter(io_scheduler* scheduler) : scheduler_(scheduler) {}
  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) noexcept;
  void await_resume() noexcept {}

 private:
  io_scheduler* scheduler_;
};

class timer_awaiter {
 public:
  timer_awaiter(io_scheduler* scheduler,
                std::chrono::steady_clock::time_point deadline,
                cancellation_token token = {})
      : scheduler_(scheduler), deadline_(deadline), token_(std::move(token)) {}
  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) noexcept;
  void await_resume();

 private:
  io_scheduler* scheduler_;
  std::chrono::steady_clock::time_point deadline_;
  cancellation_token token_;
  cancellation_registration registration_;
  std::shared_ptr<io_scheduler::timer_state> state_;
};

namespace detail {
struct detached_task {
  struct promise_type {
    detached_task get_return_object() noexcept {
      return detached_task{std::coroutine_handle<promise_type>::from_promise(*this)};
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
  std::coroutine_handle<promise_type> handle_;
};
}  // namespace detail

inline io_scheduler::io_scheduler() {
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ == -1) {
    throw std::system_error(errno, std::system_category(), "epoll_create1 failed");
  }

  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ == -1) {
    throw std::system_error(errno, std::system_category(), "eventfd failed");
  }

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.u64 = kWakeTag;
  if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) == -1) {
    throw std::system_error(errno, std::system_category(), "epoll_ctl add wake_fd failed");
  }
}

inline io_scheduler::~io_scheduler() {
  stop();
  if (wake_fd_ != -1) {
    ::close(wake_fd_);
  }
  if (epoll_fd_ != -1) {
    ::close(epoll_fd_);
  }
}

inline void io_scheduler::run() {
  if (event_loop_.joinable()) {
    return;
  }
  stopped_.store(false, std::memory_order_relaxed);
  event_loop_ = std::jthread([this](std::stop_token) { event_loop(); });
}

inline void io_scheduler::run_in_current_thread() {
  if (event_loop_.joinable()) {
    return;
  }
  stopped_.store(false, std::memory_order_relaxed);
  event_loop();
}

inline void io_scheduler::stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  wake();
  if (event_loop_.joinable()) {
    event_loop_.join();
  }
}

inline void io_scheduler::wake() noexcept {
  if (wake_fd_ == -1) {
    return;
  }
  std::uint64_t one = 1;
  ssize_t n = ::write(wake_fd_, &one, sizeof(one));
  (void)n;
}

inline void io_scheduler::enqueue_ready(std::coroutine_handle<> h) noexcept {
  std::lock_guard<std::mutex> guard(ready_mu_);
  ready_.push(h);
}

inline int io_scheduler::calc_timeout_ms() noexcept {
  std::lock_guard<std::mutex> guard(timer_mu_);
  if (timers_.empty()) {
    return -1;
  }
  auto now = std::chrono::steady_clock::now();
  auto deadline = timers_.top().deadline;
  if (deadline <= now) {
    return 0;
  }
  auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<int>(diff.count());
}

inline void io_scheduler::drain_timers() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::shared_ptr<timer_state>> ready;

  {
    std::lock_guard<std::mutex> guard(timer_mu_);
    while (!timers_.empty() && timers_.top().deadline <= now) {
      ready.push_back(timers_.top().state);
      timers_.pop();
    }
  }

  for (auto& state : ready) {
    if (state) {
      state->resume_once();
    }
  }
}

inline void io_scheduler::drain_ready() {
  std::queue<std::coroutine_handle<>> local;
  {
    std::lock_guard<std::mutex> guard(ready_mu_);
    std::swap(local, ready_);
  }
  while (!local.empty()) {
    auto h = local.front();
    local.pop();
    if (h) {
      h.resume();
    }
  }
}

inline void io_scheduler::event_loop() {
  constexpr int kMaxEvents = 64;
  std::array<epoll_event, kMaxEvents> events{};

  while (!stopped_.load(std::memory_order_relaxed)) {
    int timeout_ms = calc_timeout_ms();
    int n = epoll_wait(epoll_fd_, events.data(), kMaxEvents, timeout_ms);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    for (int i = 0; i < n; ++i) {
      auto tag = events[i].data.u64;
      if (tag == kWakeTag) {
        std::uint64_t buf = 0;
        while (::read(wake_fd_, &buf, sizeof(buf)) > 0) {
        }
        continue;
      }

      auto* awaiter = reinterpret_cast<io_awaiter*>(tag);
      if (awaiter) {
        awaiter->on_event(events[i].events);
      }
    }

    drain_timers();
    drain_ready();
  }

  drain_timers();
  drain_ready();
  stopped_.store(true, std::memory_order_relaxed);
}

inline task<> io_scheduler::schedule() {
  co_await schedule_awaiter{this};
}

inline task<size_t> io_scheduler::async_read_some(int fd, void* buffer, size_t count) {
  co_return co_await async_read_some(fd, buffer, count, cancellation_token{});
}

inline task<size_t> io_scheduler::async_read_exact(int fd, void* buffer, size_t count) {
  co_return co_await async_read_exact(fd, buffer, count, cancellation_token{});
}

inline task<size_t> io_scheduler::async_write_all(int fd, const void* buffer, size_t count) {
  co_return co_await async_write_all(fd, buffer, count, cancellation_token{});
}

inline task<size_t> io_scheduler::async_read_some(int fd, void* buffer, size_t count,
                                                  cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  std::byte* p = static_cast<std::byte*>(buffer);
  size_t total = 0;

  for (;;) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::read(fd, p + total, count - total);
    if (n > 0) {
      total += static_cast<size_t>(n);
      co_return total;
    }
    if (n == 0) {
      co_return total;  // EOF
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await io_awaiter{this, fd, io_event::READ, token};
      continue;
    }
    throw std::system_error(errno, std::system_category(), "read failed");
  }
}

inline task<size_t> io_scheduler::async_read_exact(int fd, void* buffer, size_t count,
                                                   cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  std::byte* p = static_cast<std::byte*>(buffer);
  size_t total = 0;

  while (total < count) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::read(fd, p + total, count - total);
    if (n > 0) {
      total += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      co_return total;  // EOF
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await io_awaiter{this, fd, io_event::READ, token};
      continue;
    }
    throw std::system_error(errno, std::system_category(), "read failed");
  }

  co_return total;
}

inline task<size_t> io_scheduler::async_write_all(int fd, const void* buffer, size_t count,
                                                  cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  const std::byte* p = static_cast<const std::byte*>(buffer);
  size_t written = 0;

  while (written < count) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::write(fd, p + written, count - written);
    if (n > 0) {
      written += static_cast<size_t>(n);
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await io_awaiter{this, fd, io_event::WRITE, token};
      continue;
    }
    throw std::system_error(errno, std::system_category(), "write failed");
  }

  co_return written;
}

inline task<size_t> io_scheduler::async_read(int fd, void* buffer, size_t count) {
  co_return co_await async_read_some(fd, buffer, count);
}

inline task<size_t> io_scheduler::async_write(int fd, const void* buffer, size_t count) {
  co_return co_await async_write_all(fd, buffer, count);
}

inline task<size_t> io_scheduler::async_read(int fd, void* buffer, size_t count,
                                             cancellation_token token) {
  co_return co_await async_read_some(fd, buffer, count, token);
}

inline task<size_t> io_scheduler::async_write(int fd, const void* buffer, size_t count,
                                              cancellation_token token) {
  co_return co_await async_write_all(fd, buffer, count, token);
}

inline task<size_t> io_scheduler::async_read(socket& sock, buffer& buf, size_t max_bytes) {
  co_return co_await async_read_some(sock, buf, max_bytes);
}

inline task<size_t> io_scheduler::async_write(socket& sock, buffer& buf, size_t max_bytes) {
  co_return co_await async_write_all(sock, buf, max_bytes);
}

inline task<size_t> io_scheduler::async_read(socket& sock, buffer& buf, size_t max_bytes,
                                             cancellation_token token) {
  co_return co_await async_read_some(sock, buf, max_bytes, token);
}

inline task<size_t> io_scheduler::async_write(socket& sock, buffer& buf, size_t max_bytes,
                                              cancellation_token token) {
  co_return co_await async_write_all(sock, buf, max_bytes, token);
}

inline task<size_t> io_scheduler::async_read_some(socket& sock, buffer& buf, size_t max_bytes) {
  if (max_bytes == 0) {
    max_bytes = buf.writable_bytes();
    if (max_bytes == 0) {
      max_bytes = kDefaultReadSize;
    }
  }
  buf.ensure_writable(max_bytes);
  size_t n = co_await async_read_some(sock.fd(), buf.write_ptr(), max_bytes);
  buf.has_written(n);
  co_return n;
}

inline task<size_t> io_scheduler::async_read_exact(socket& sock, buffer& buf, size_t count) {
  if (count == 0) {
    co_return 0;
  }
  buf.ensure_writable(count);
  size_t n = co_await async_read_exact(sock.fd(), buf.write_ptr(), count);
  buf.has_written(n);
  co_return n;
}

inline task<size_t> io_scheduler::async_write_all(socket& sock, buffer& buf, size_t max_bytes) {
  size_t to_write = buf.readable_bytes();
  if (max_bytes != 0 && max_bytes < to_write) {
    to_write = max_bytes;
  }
  if (to_write == 0) {
    co_return 0;
  }
  size_t n = co_await async_write_all(sock.fd(), buf.read_ptr(), to_write);
  buf.has_read(n);
  co_return n;
}

inline task<size_t> io_scheduler::async_read_some(socket& sock, buffer& buf, size_t max_bytes,
                                                  cancellation_token token) {
  if (max_bytes == 0) {
    max_bytes = buf.writable_bytes();
    if (max_bytes == 0) {
      max_bytes = kDefaultReadSize;
    }
  }
  buf.ensure_writable(max_bytes);
  size_t n = co_await async_read_some(sock.fd(), buf.write_ptr(), max_bytes, token);
  buf.has_written(n);
  co_return n;
}

inline task<size_t> io_scheduler::async_read_exact(socket& sock, buffer& buf, size_t count,
                                                   cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  buf.ensure_writable(count);
  size_t n = co_await async_read_exact(sock.fd(), buf.write_ptr(), count, token);
  buf.has_written(n);
  co_return n;
}

inline task<size_t> io_scheduler::async_write_all(socket& sock, buffer& buf, size_t max_bytes,
                                                  cancellation_token token) {
  size_t to_write = buf.readable_bytes();
  if (max_bytes != 0 && max_bytes < to_write) {
    to_write = max_bytes;
  }
  if (to_write == 0) {
    co_return 0;
  }
  size_t n = co_await async_write_all(sock.fd(), buf.read_ptr(), to_write, token);
  buf.has_read(n);
  co_return n;
}

inline task<> io_scheduler::async_accept(int fd) {
  co_await io_awaiter{this, fd, io_event::READ};
}

inline task<std::pair<socket, std::shared_ptr<address>>> io_scheduler::async_accept(
    socket& listen_sock) {
  for (;;) {
    sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = ::accept4(listen_sock.fd(), reinterpret_cast<sockaddr*>(&client_addr),
                              &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd >= 0) {
      auto client_addr_obj =
          address::from_sockaddr(reinterpret_cast<sockaddr*>(&client_addr), addr_len);
      co_return std::pair<socket, std::shared_ptr<address>>{socket{client_fd}, client_addr_obj};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await io_awaiter{this, listen_sock.fd(), io_event::READ};
      continue;
    }
    throw std::system_error(errno, std::system_category(), "accept failed");
  }
}

inline task<> io_scheduler::async_accept(int fd, cancellation_token token) {
  co_await io_awaiter{this, fd, io_event::READ, token};
}

inline task<std::pair<socket, std::shared_ptr<address>>> io_scheduler::async_accept(
    socket& listen_sock, cancellation_token token) {
  for (;;) {
    throw_if_cancellation_requested(token);
    sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = ::accept4(listen_sock.fd(), reinterpret_cast<sockaddr*>(&client_addr),
                              &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client_fd >= 0) {
      auto client_addr_obj =
          address::from_sockaddr(reinterpret_cast<sockaddr*>(&client_addr), addr_len);
      co_return std::pair<socket, std::shared_ptr<address>>{socket{client_fd}, client_addr_obj};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      co_await io_awaiter{this, listen_sock.fd(), io_event::READ, token};
      continue;
    }
    throw std::system_error(errno, std::system_category(), "accept failed");
  }
}

inline task<> io_scheduler::async_connection(int fd) {
  co_await io_awaiter{this, fd, io_event::WRITE};
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
    throw std::system_error(errno, std::system_category(), "getsockopt SO_ERROR failed");
  }
  if (err != 0) {
    throw std::system_error(err, std::system_category(), "connect failed");
  }
}

inline task<> io_scheduler::async_connection(int fd, cancellation_token token) {
  co_await io_awaiter{this, fd, io_event::WRITE, token};
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
    throw std::system_error(errno, std::system_category(), "getsockopt SO_ERROR failed");
  }
  if (err != 0) {
    throw std::system_error(err, std::system_category(), "connect failed");
  }
}

inline task<> io_scheduler::async_connect(socket& sock, const address& addr) {
  for (;;) {
    int r = ::connect(sock.fd(), addr.addr(), addr.addr_len());
    if (r == 0) {
      co_return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EINPROGRESS || errno == EALREADY) {
      co_await io_awaiter{this, sock.fd(), io_event::WRITE};
      int err = 0;
      socklen_t len = sizeof(err);
      if (::getsockopt(sock.fd(), SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
        throw std::system_error(errno, std::system_category(), "getsockopt SO_ERROR failed");
      }
      if (err != 0) {
        throw std::system_error(err, std::system_category(), "connect failed");
      }
      co_return;
    }
    throw std::system_error(errno, std::system_category(), "connect failed");
  }
}

inline task<> io_scheduler::async_connect(socket& sock, const address& addr,
                                          cancellation_token token) {
  for (;;) {
    throw_if_cancellation_requested(token);
    int r = ::connect(sock.fd(), addr.addr(), addr.addr_len());
    if (r == 0) {
      co_return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EINPROGRESS || errno == EALREADY) {
      co_await io_awaiter{this, sock.fd(), io_event::WRITE, token};
      int err = 0;
      socklen_t len = sizeof(err);
      if (::getsockopt(sock.fd(), SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
        throw std::system_error(errno, std::system_category(), "getsockopt SO_ERROR failed");
      }
      if (err != 0) {
        throw std::system_error(err, std::system_category(), "connect failed");
      }
      co_return;
    }
    throw std::system_error(errno, std::system_category(), "connect failed");
  }
}

template <typename T>
inline void io_scheduler::spawn(task<T> t) {
  auto starter = [this](task<T> inner) -> detail::detached_task {
    co_await schedule();
    co_await std::move(inner);
  };
  (void)starter(std::move(t));
}

inline void io_awaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
  handle_ = handle;
  if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
    cancelled_.store(true, std::memory_order_release);
    resume_once();
    scheduler_->wake();
    return;
  }
  std::uint32_t ev = static_cast<std::uint32_t>(
      events_ | io_event::ONESHOT | io_event::RD_HANGUUP);
  epoll_event event{};
  event.events = ev;
  event.data.u64 = reinterpret_cast<std::uint64_t>(this);

  if (epoll_ctl(scheduler_->epoll_fd_, EPOLL_CTL_MOD, fd_, &event) == -1) {
    if (errno == ENOENT) {
      if (epoll_ctl(scheduler_->epoll_fd_, EPOLL_CTL_ADD, fd_, &event) == -1) {
        error_ = errno;
      }
    } else {
      error_ = errno;
    }
  }

  if (token_.can_be_cancelled()) {
    registration_ = cancellation_registration(
        token_, [this]() noexcept {
          cancelled_.store(true, std::memory_order_release);
          epoll_ctl(scheduler_->epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
          resume_once();
          scheduler_->wake();
        });
  }

  if (error_ != 0) {
    resume_once();
    scheduler_->wake();
  }
}

inline void io_awaiter::await_resume() {
  if (cancelled_.load(std::memory_order_acquire)) {
    throw operation_cancelled{};
  }
  if (error_ != 0) {
    throw std::system_error(error_, std::system_category(), "epoll_ctl failed");
  }
}

inline void io_awaiter::on_event(std::uint32_t) noexcept {
  resume_once();
}

inline void io_awaiter::resume_once() noexcept {
  if (!completed_.exchange(true, std::memory_order_acq_rel)) {
    scheduler_->enqueue_ready(handle_);
  }
}

inline void schedule_awaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
  scheduler_->enqueue_ready(handle);
  scheduler_->wake();
}

inline void timer_awaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
  state_ = std::make_shared<io_scheduler::timer_state>();
  state_->scheduler = scheduler_;
  state_->handle = handle;

  if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
    state_->cancelled.store(true, std::memory_order_release);
    state_->resume_once();
    scheduler_->wake();
    return;
  }

  if (token_.can_be_cancelled()) {
    registration_ = cancellation_registration(
        token_, [state = state_, scheduler = scheduler_]() noexcept {
          state->cancelled.store(true, std::memory_order_release);
          state->resume_once();
          scheduler->wake();
        });
  }

  {
    std::lock_guard<std::mutex> guard(scheduler_->timer_mu_);
    scheduler_->timers_.push(io_scheduler::timer_item{deadline_, state_});
  }
  scheduler_->wake();
}

inline void timer_awaiter::await_resume() {
  if (state_ && state_->cancelled.load(std::memory_order_acquire)) {
    throw operation_cancelled{};
  }
}

template <typename Rep, typename Period>
inline task<> io_scheduler::sleep_for(std::chrono::duration<Rep, Period> duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  co_await timer_awaiter{this, deadline};
}

template <typename Rep, typename Period>
inline task<> io_scheduler::sleep_for(std::chrono::duration<Rep, Period> duration,
                                      cancellation_token token) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  co_await timer_awaiter{this, deadline, token};
}

}  // namespace xcoro::net
