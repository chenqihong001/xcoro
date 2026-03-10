# 网络模块重构设计（xcoro）

本文档是网络层的重写目标说明。  
目标是“借鉴 Asio 的 reactor + endpoint 思路”，但接口全部走现代 C++ 协程风格，不保留 callback 历史包袱。

本文涉及的网络组件：

- `io_context`（事件循环）
- `endpoint` / `address`（地址与端点）
- `socket`（统一连接态与流式 I/O，融合原 `tcp_stream`）
- `acceptor`（替代 `tcp_acceptor` 命名）
- `resolver`（域名解析）
- `buffer`（重写为“拥有型 + 视图型”）

取消语义统一使用现有 cancellation 机制：

- `cancellation_source`
- `cancellation_token`
- `cancellation_registration`
- `operation_cancelled`

不引入 `cancel_reason`。

---

## 1. 设计原则

1. 所有异步网络接口均为 `co_await` 风格，返回 `task<T>`。
2. 不提供 callback 版本 API，不做“双轨接口”维护。
3. I/O 等待全部基于非阻塞 fd + reactor（epoll）。
4. 取消是协作式请求：在取消点观测，抛 `operation_cancelled`。
5. 统一命名：`acceptor`、`socket`、`endpoint`，避免 `tcp_*` 前缀泛滥。

---

## 2. 总体模型（Asio 思路，协程优先）

### 2.1 线程与执行模型

- 事件循环对象负责等待 fd 就绪并恢复协程。
- 协程恢复在事件循环线程执行（或显式调度到指定执行器）。
- I/O 操作遵循“系统调用失败且 `EAGAIN/EWOULDBLOCK` -> 挂起等待 -> 继续重试”。

### 2.2 错误模型

- 取消：抛 `operation_cancelled`。
- 系统调用错误：抛 `std::system_error`。
- EOF：`async_read_some` 返回 `0`，不抛异常。

---

## 3. 取消语义（网络层强约束）

所有可阻塞的异步操作都必须提供 token 重载，且行为一致：

```cpp
task<std::size_t> socket::async_read_some(mutable_buffer dst);
task<std::size_t> socket::async_read_some(mutable_buffer dst, cancellation_token token);

task<socket> acceptor::async_accept();
task<socket> acceptor::async_accept(cancellation_token token);

template <typename Rep, typename Period>
task<> io_context::sleep_for(std::chrono::duration<Rep, Period> d, cancellation_token token);
```

统一约束：

1. 进入等待前先检查 token（快速失败）。
2. 挂起时注册取消回调。
3. 事件完成与取消完成竞争“单次恢复权”（`completed_` 原子位）。
4. `await_resume()` 统一将取消转换为 `operation_cancelled`。

这与 `9.1` 里的 `io_awaiter` / `timer_awaiter` 机制一致，后续网络组件都复用同一语义。

---

## 4. 组件设计

### 4.1 io_context（事件循环核心）

`io_context` 直接承载 reactor 事件循环，不依赖旧的 `io_scheduler` 别名层：

```cpp
namespace xcoro::net {
class io_context {
 public:
  void run();
  void run_in_current_thread();
  void stop();

  task<> schedule();
  template <typename T>
  void spawn(task<T> t);

  template <class Rep, class Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> d);

  template <class Rep, class Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> d, cancellation_token token);
  
  task<std::size_t> async_read_some(int fd, void* buffer, std::size_t count,
                                    cancellation_token token = {});
  task<std::size_t> async_read_exact(int fd, void* buffer, std::size_t count,
                                     cancellation_token token = {});
  task<std::size_t> async_write_all(int fd, const void* buffer, std::size_t count,
                                    cancellation_token token = {});

  task<> async_accept(int fd, cancellation_token token = {});
  task<> async_connection(int fd, cancellation_token token = {});
};
} // namespace xcoro::net
```

实现使用 epoll + eventfd + ready queue + timer heap，不需要回调接口。
完整实现见 `9.1`。

### 4.2 endpoint/address（值语义优先）

当前 `address` 使用虚基类 + `shared_ptr`，可工作但使用成本偏高。  
重写目标是 value type `endpoint`（更贴近 Asio endpoint）：

```cpp
class endpoint {
 public:
  static endpoint from_ip_port(std::string_view ip, uint16_t port);
  static endpoint from_sockaddr(const sockaddr* sa, socklen_t len);

  const sockaddr* data() const noexcept;
  socklen_t size() const noexcept;
  int family() const noexcept;

  std::string ip() const;
  uint16_t port() const noexcept;
  std::string to_string() const;
};
```

保留点：

- 支持 IPv4 / IPv6。
- 支持 `"[::1]"` 这种文本输入。

收益：

- 不再到处传 `shared_ptr<address>`。
- `accept/connect/bind` API 更直接，避免堆分配和虚调用。
完整实现见 `9.2`。

### 4.3 buffer（重写）

当前 `buffer`（含 `ring_buffer`）接口偏“容器导向”，但和异步 I/O 协作时不够统一。  
建议拆成两层：

1. 拥有型 `byte_buffer`（维护读写游标）
2. 非拥有型 `mutable_buffer` / `const_buffer`（`std::span<std::byte>`）

```cpp
class byte_buffer {
 public:
  explicit byte_buffer(std::size_t initial = 4096);

  std::size_t size() const noexcept;      // 可读字节
  std::size_t capacity() const noexcept;

  std::span<const std::byte> data() const noexcept;   // 可读区域
  std::span<std::byte> prepare(std::size_t n);        // 预留写入区域
  void commit(std::size_t n) noexcept;                // 写入完成
  void consume(std::size_t n) noexcept;               // 读取完成

  void clear() noexcept;
  void shrink_to_fit();
};

struct mutable_buffer {
  std::span<std::byte> bytes;
};

struct const_buffer {
  std::span<const std::byte> bytes;
};
```

接口风格更贴近 Asio dynamic buffer 思路，但去掉 callback 配套 API，只保留协程用法。
完整实现见 `9.3`。

### 4.4 socket（融合 `tcp_stream`）

重写目标：`socket` 既是 RAII fd 封装，也是协程 I/O 主入口。  
`tcp_stream` 删除，能力并入 `socket`。

```cpp
class socket {
 public:
  socket() = default;
  socket(io_context& ctx, int fd);

  static socket open_tcp(io_context& ctx, int family = AF_INET);
  static socket open_udp(io_context& ctx, int family = AF_INET);

  void set_nonblocking(bool on = true);
  void set_reuse_addr(bool on = true);
  void set_reuse_port(bool on = true);
  void set_tcp_nodelay(bool on = true);

  void bind(const endpoint& ep);
  void listen(int backlog = SOMAXCONN);
  task<> async_connect(const endpoint& ep);
  task<> async_connect(const endpoint& ep, cancellation_token token);

  task<std::size_t> async_read_some(mutable_buffer buf);
  task<std::size_t> async_read_some(mutable_buffer buf, cancellation_token token);
  task<std::size_t> async_read_exact(mutable_buffer buf);
  task<std::size_t> async_read_exact(mutable_buffer buf, cancellation_token token);

  task<std::size_t> async_write_some(const_buffer buf);
  task<std::size_t> async_write_some(const_buffer buf, cancellation_token token);
  task<std::size_t> async_write_all(const_buffer buf);
  task<std::size_t> async_write_all(const_buffer buf, cancellation_token token);

  int native_handle() const noexcept;
  bool is_open() const noexcept;
  void close() noexcept;
};
```

说明：

- `socket` 必须绑定 `io_context` 才能调用 async 成员函数。
- 对外暴露语义明确的 read/write 变体，不再保留模糊 `async_read/async_write`。
完整实现见 `9.4`。

### 4.5 acceptor（替代 `tcp_acceptor`）

保留“监听职责”这个独立组件，但去掉 `tcp_` 前缀：

```cpp
class acceptor {
 public:
  static acceptor bind(io_context& ctx, const endpoint& ep, int backlog = SOMAXCONN);

  socket& native_socket() noexcept;
  task<socket> async_accept();
  task<socket> async_accept(cancellation_token token);
};
```

默认行为：

- 自动设置 `SO_REUSEADDR` / `SO_REUSEPORT` / nonblocking。
- `async_accept(token)` 取消时抛 `operation_cancelled`。
完整实现见 `9.5`。

### 4.6 resolver

保留同步解析入口，同时增加协程异步入口（内部可投递到后台执行器）：

```cpp
struct resolve_options {
  int family = AF_UNSPEC;
  int socktype = SOCK_STREAM;
  int flags = AI_ADDRCONFIG;
};

class resolver {
 public:
  static std::vector<endpoint> resolve(std::string_view host,
                                       std::string_view service,
                                       resolve_options opt = {});

  static task<std::vector<endpoint>> async_resolve(io_context& ctx,
                                                   std::string host,
                                                   std::string service,
                                                   resolve_options opt = {},
                                                   cancellation_token token = {});
};
```

完整实现见 `9.6`。

---

## 5. 关键流程（取消与 I/O 一体化）

### 5.1 读取流程

1. 调用 `::read`。
2. `EINTR` 重试。
3. `EAGAIN/EWOULDBLOCK` 时挂起等待 `READ` 事件。
4. token 被取消时，等待立即完成并抛 `operation_cancelled`。

### 5.2 连接流程

1. `::connect` 立即成功直接返回。
2. `EINPROGRESS/EALREADY` 挂起等待 `WRITE` 事件。
3. 恢复后 `getsockopt(SO_ERROR)` 校验结果。
4. token 取消时中断等待并抛 `operation_cancelled`。

### 5.3 accept 流程

1. 循环 `accept4`。
2. `EAGAIN/EWOULDBLOCK` 挂起等待监听 fd 可读。
3. 成功后返回新 `socket`（已是 nonblocking + cloexec）。
4. token 取消时抛 `operation_cancelled`。

---

## 6. 推荐接口用法（示例）

```cpp
using namespace xcoro::net;

task<> echo_session(socket client, cancellation_token token) {
  byte_buffer buf(4096);

  for (;;) {
    auto writable = buf.prepare(4096);
    auto n = co_await client.async_read_some(mutable_buffer{writable}, token);
    if (n == 0) {
      co_return; // peer closed
    }
    buf.commit(n);

    auto readable = buf.data();
    co_await client.async_write_all(const_buffer{readable}, token);
    buf.consume(readable.size());
  }
}

task<> run_server(io_context& ctx, uint16_t port, cancellation_token token) {
  auto ep = endpoint::from_ip_port("0.0.0.0", port);
  auto a = acceptor::bind(ctx, ep);

  for (;;) {
    auto client = co_await a.async_accept(token);
    ctx.spawn(echo_session(std::move(client), token));
  }
}
```

---

## 7. 迁移映射（现有代码 -> 目标接口）

- `tcp_stream` -> 合并到 `socket` async 成员函数。
- `tcp_acceptor` -> 重命名并简化为 `acceptor`。
- `address`（多态 + `shared_ptr`）-> `endpoint` 值类型。
- `buffer` / `ring_buffer` -> `byte_buffer` + `mutable_buffer` / `const_buffer`。
- 事件循环统一收敛到 `io_context`，不再保留 `io_scheduler`。

---

## 8. 落地顺序建议

1. 先落地 `io_context` 独立实现（epoll/eventfd/timer）。
2. 合并 `tcp_stream` 到 `socket`，统一走 `io_context` fd 级接口。
3. 引入 `endpoint` 值类型并逐步替换 `shared_ptr<address>`。
4. 引入新 `byte_buffer`，旧 `buffer` 暂时保留兼容适配层。
5. 最后清理旧 API（`tcp_*` 与模糊 read/write 别名）。

---

## 9. 参考实现代码（完整）

这一节给的是完整参考实现片段（header-only 风格）。  
你可以按这里的文件拆分写进项目，再按自己的命名细节调整。

### 9.1 `io_context`（完整实现，独立 epoll 版）

```cpp
// include/xcoro/net/io_context.hpp
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

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"
#include "xcoro/task.hpp"

namespace xcoro::net {

enum io_event {
  READ = EPOLLIN,
  WRITE = EPOLLOUT,
  ERROR = EPOLLERR,
  HANGUP = EPOLLHUP,
  RD_HANGUP = EPOLLRDHUP,
  ONESHOT = EPOLLONESHOT
};

class io_awaiter;
class schedule_awaiter;
class timer_awaiter;

class io_context {
 public:
  io_context();
  ~io_context();

  io_context(const io_context&) = delete;
  io_context& operator=(const io_context&) = delete;
  io_context(io_context&&) = delete;
  io_context& operator=(io_context&&) = delete;

  void run();
  void run_in_current_thread();
  void stop();

  task<> schedule();

  task<std::size_t> async_read_some(int fd, void* buffer, std::size_t count);
  task<std::size_t> async_read_some(int fd, void* buffer, std::size_t count,
                                    cancellation_token token);

  task<std::size_t> async_read_exact(int fd, void* buffer, std::size_t count);
  task<std::size_t> async_read_exact(int fd, void* buffer, std::size_t count,
                                     cancellation_token token);

  task<std::size_t> async_write_all(int fd, const void* buffer, std::size_t count);
  task<std::size_t> async_write_all(int fd, const void* buffer, std::size_t count,
                                    cancellation_token token);

  task<> async_accept(int fd, cancellation_token token = {});
  task<> async_connection(int fd, cancellation_token token = {});

  template <typename T>
  void spawn(task<T> t);

  template <typename Rep, typename Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> d);

  template <typename Rep, typename Period>
  task<> sleep_for(std::chrono::duration<Rep, Period> d, cancellation_token token);

 private:
  friend class io_awaiter;
  friend class schedule_awaiter;
  friend class timer_awaiter;

  static constexpr std::uint64_t kWakeTag = 0xffffffffffffffffULL;

  void enqueue_ready(std::coroutine_handle<> h) noexcept;
  void wake() noexcept;
  void event_loop();
  void drain_ready();
  void drain_timers();
  int calc_timeout_ms() noexcept;

  struct timer_state {
    io_context* ctx{};
    std::coroutine_handle<> handle{};
    std::atomic<bool> completed{false};
    std::atomic<bool> cancelled{false};

    void resume_once() noexcept {
      if (!completed.exchange(true, std::memory_order_acq_rel)) {
        ctx->enqueue_ready(handle);
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
  std::jthread loop_thread_;
  std::atomic<bool> stopped_{false};

  std::mutex ready_mu_;
  std::queue<std::coroutine_handle<>> ready_;

  std::mutex timer_mu_;
  std::priority_queue<timer_item, std::vector<timer_item>, timer_cmp> timers_;
};

class io_awaiter {
 public:
  io_awaiter(io_context* ctx, int fd, int events, cancellation_token token = {})
      : ctx_(ctx), fd_(fd), events_(events), token_(std::move(token)) {}

  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept;
  void await_resume();

  void on_event(std::uint32_t) noexcept;

 private:
  void resume_once() noexcept;

  io_context* ctx_{};
  int fd_{-1};
  int events_{0};
  std::coroutine_handle<> handle_{};
  int error_{0};
  std::atomic<bool> completed_{false};
  std::atomic<bool> cancelled_{false};
  cancellation_token token_;
  cancellation_registration reg_;
};

class schedule_awaiter {
 public:
  explicit schedule_awaiter(io_context* ctx) : ctx_(ctx) {}
  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept;
  void await_resume() noexcept {}

 private:
  io_context* ctx_{};
};

class timer_awaiter {
 public:
  timer_awaiter(io_context* ctx,
                std::chrono::steady_clock::time_point deadline,
                cancellation_token token = {})
      : ctx_(ctx), deadline_(deadline), token_(std::move(token)) {}

  bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept;
  void await_resume();

 private:
  io_context* ctx_{};
  std::chrono::steady_clock::time_point deadline_;
  cancellation_token token_;
  cancellation_registration reg_;
  std::shared_ptr<io_context::timer_state> state_;
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

  explicit detached_task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
  detached_task(detached_task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
  detached_task& operator=(detached_task&& other) noexcept {
    if (this != &other) {
      h_ = std::exchange(other.h_, {});
    }
    return *this;
  }
  ~detached_task() = default;

 private:
  std::coroutine_handle<promise_type> h_{};
};
} // namespace detail

inline io_context::io_context() {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ == -1) {
    throw std::system_error(errno, std::system_category(), "epoll_create1 failed");
  }

  wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ == -1) {
    throw std::system_error(errno, std::system_category(), "eventfd failed");
  }

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.u64 = kWakeTag;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev) == -1) {
    throw std::system_error(errno, std::system_category(), "epoll_ctl add wake_fd failed");
  }
}

inline io_context::~io_context() {
  stop();
  if (wake_fd_ != -1) {
    ::close(wake_fd_);
  }
  if (epoll_fd_ != -1) {
    ::close(epoll_fd_);
  }
}

inline void io_context::run() {
  if (loop_thread_.joinable()) {
    return;
  }
  stopped_.store(false, std::memory_order_relaxed);
  loop_thread_ = std::jthread([this](std::stop_token) { event_loop(); });
}

inline void io_context::run_in_current_thread() {
  if (loop_thread_.joinable()) {
    return;
  }
  stopped_.store(false, std::memory_order_relaxed);
  event_loop();
}

inline void io_context::stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  wake();
  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
}

inline void io_context::wake() noexcept {
  if (wake_fd_ == -1) {
    return;
  }
  std::uint64_t one = 1;
  (void)::write(wake_fd_, &one, sizeof(one));
}

inline void io_context::enqueue_ready(std::coroutine_handle<> h) noexcept {
  std::lock_guard<std::mutex> lock(ready_mu_);
  ready_.push(h);
}

inline int io_context::calc_timeout_ms() noexcept {
  std::lock_guard<std::mutex> lock(timer_mu_);
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

inline void io_context::drain_timers() {
  auto now = std::chrono::steady_clock::now();
  std::vector<std::shared_ptr<timer_state>> due;

  {
    std::lock_guard<std::mutex> lock(timer_mu_);
    while (!timers_.empty() && timers_.top().deadline <= now) {
      due.push_back(timers_.top().state);
      timers_.pop();
    }
  }

  for (auto& s : due) {
    if (s) {
      s->resume_once();
    }
  }
}

inline void io_context::drain_ready() {
  std::queue<std::coroutine_handle<>> q;
  {
    std::lock_guard<std::mutex> lock(ready_mu_);
    std::swap(q, ready_);
  }
  while (!q.empty()) {
    auto h = q.front();
    q.pop();
    if (h) {
      h.resume();
    }
  }
}

inline void io_context::event_loop() {
  constexpr int kMaxEvents = 64;
  std::array<epoll_event, kMaxEvents> evs{};

  while (!stopped_.load(std::memory_order_relaxed)) {
    int n = ::epoll_wait(epoll_fd_, evs.data(), kMaxEvents, calc_timeout_ms());
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    for (int i = 0; i < n; ++i) {
      if (evs[i].data.u64 == kWakeTag) {
        std::uint64_t buf = 0;
        while (::read(wake_fd_, &buf, sizeof(buf)) > 0) {
        }
        continue;
      }

      auto* aw = reinterpret_cast<io_awaiter*>(evs[i].data.u64);
      if (aw) {
        aw->on_event(evs[i].events);
      }
    }

    drain_timers();
    drain_ready();
  }

  drain_timers();
  drain_ready();
  stopped_.store(true, std::memory_order_relaxed);
}

inline task<> io_context::schedule() {
  co_await schedule_awaiter{this};
}

inline task<std::size_t> io_context::async_read_some(int fd, void* buffer, std::size_t count) {
  co_return co_await async_read_some(fd, buffer, count, cancellation_token{});
}

inline task<std::size_t> io_context::async_read_exact(int fd, void* buffer, std::size_t count) {
  co_return co_await async_read_exact(fd, buffer, count, cancellation_token{});
}

inline task<std::size_t> io_context::async_write_all(int fd, const void* buffer, std::size_t count) {
  co_return co_await async_write_all(fd, buffer, count, cancellation_token{});
}

inline task<std::size_t> io_context::async_read_some(int fd, void* buffer, std::size_t count,
                                                     cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  auto* p = static_cast<std::byte*>(buffer);

  for (;;) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::read(fd, p, count);
    if (n > 0) {
      co_return static_cast<std::size_t>(n);
    }
    if (n == 0) {
      co_return 0;
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

inline task<std::size_t> io_context::async_read_exact(int fd, void* buffer, std::size_t count,
                                                      cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  auto* p = static_cast<std::byte*>(buffer);
  std::size_t total = 0;

  while (total < count) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::read(fd, p + total, count - total);
    if (n > 0) {
      total += static_cast<std::size_t>(n);
      continue;
    }
    if (n == 0) {
      co_return total;
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

inline task<std::size_t> io_context::async_write_all(int fd, const void* buffer, std::size_t count,
                                                     cancellation_token token) {
  if (count == 0) {
    co_return 0;
  }
  auto* p = static_cast<const std::byte*>(buffer);
  std::size_t written = 0;

  while (written < count) {
    throw_if_cancellation_requested(token);
    ssize_t n = ::write(fd, p + written, count - written);
    if (n > 0) {
      written += static_cast<std::size_t>(n);
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

inline task<> io_context::async_accept(int fd, cancellation_token token) {
  co_await io_awaiter{this, fd, io_event::READ, token};
}

inline task<> io_context::async_connection(int fd, cancellation_token token) {
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

template <typename T>
inline void io_context::spawn(task<T> t) {
  auto starter = [this](task<T> inner) -> detail::detached_task {
    co_await schedule();
    co_await std::move(inner);
  };
  (void)starter(std::move(t));
}

template <typename Rep, typename Period>
inline task<> io_context::sleep_for(std::chrono::duration<Rep, Period> d) {
  const auto deadline = std::chrono::steady_clock::now() + d;
  co_await timer_awaiter{this, deadline};
}

template <typename Rep, typename Period>
inline task<> io_context::sleep_for(std::chrono::duration<Rep, Period> d, cancellation_token token) {
  const auto deadline = std::chrono::steady_clock::now() + d;
  co_await timer_awaiter{this, deadline, token};
}

inline void io_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
  handle_ = h;
  if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
    cancelled_.store(true, std::memory_order_release);
    resume_once();
    ctx_->wake();
    return;
  }

  std::uint32_t ev = static_cast<std::uint32_t>(events_ | io_event::ONESHOT | io_event::RD_HANGUP);
  epoll_event event{};
  event.events = ev;
  event.data.u64 = reinterpret_cast<std::uint64_t>(this);

  if (::epoll_ctl(ctx_->epoll_fd_, EPOLL_CTL_MOD, fd_, &event) == -1) {
    if (errno == ENOENT) {
      if (::epoll_ctl(ctx_->epoll_fd_, EPOLL_CTL_ADD, fd_, &event) == -1) {
        error_ = errno;
      }
    } else {
      error_ = errno;
    }
  }

  if (token_.can_be_cancelled()) {
    reg_ = cancellation_registration(token_, [this]() noexcept {
      cancelled_.store(true, std::memory_order_release);
      ::epoll_ctl(ctx_->epoll_fd_, EPOLL_CTL_DEL, fd_, nullptr);
      resume_once();
      ctx_->wake();
    });
  }

  if (error_ != 0) {
    resume_once();
    ctx_->wake();
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
    ctx_->enqueue_ready(handle_);
  }
}

inline void schedule_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
  ctx_->enqueue_ready(h);
  ctx_->wake();
}

inline void timer_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
  state_ = std::make_shared<io_context::timer_state>();
  state_->ctx = ctx_;
  state_->handle = h;

  if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
    state_->cancelled.store(true, std::memory_order_release);
    state_->resume_once();
    ctx_->wake();
    return;
  }

  if (token_.can_be_cancelled()) {
    reg_ = cancellation_registration(token_, [state = state_, ctx = ctx_]() noexcept {
      state->cancelled.store(true, std::memory_order_release);
      state->resume_once();
      ctx->wake();
    });
  }

  {
    std::lock_guard<std::mutex> lock(ctx_->timer_mu_);
    ctx_->timers_.push(io_context::timer_item{deadline_, state_});
  }
  ctx_->wake();
}

inline void timer_awaiter::await_resume() {
  if (state_ && state_->cancelled.load(std::memory_order_acquire)) {
    throw operation_cancelled{};
  }
}

} // namespace xcoro::net
```

### 9.2 `endpoint`（值类型）

```cpp
// include/xcoro/net/endpoint.hpp
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xcoro::net {

class endpoint {
 public:
  endpoint() noexcept : len_(0) { std::memset(&storage_, 0, sizeof(storage_)); }

  static endpoint from_ip_port(std::string_view ip_sv, uint16_t port) {
    endpoint ep;
    std::string ip(ip_sv);
    if (!ip.empty() && ip.front() == '[' && ip.back() == ']') {
      ip = ip.substr(1, ip.size() - 2);
    }

    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
      v4.sin_addr.s_addr = htonl(INADDR_ANY);
      std::memcpy(&ep.storage_, &v4, sizeof(v4));
      ep.len_ = sizeof(v4);
      return ep;
    }
    if (::inet_pton(AF_INET, ip.c_str(), &v4.sin_addr) == 1) {
      std::memcpy(&ep.storage_, &v4, sizeof(v4));
      ep.len_ = sizeof(v4);
      return ep;
    }

    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port);
    if (ip == "::") {
      v6.sin6_addr = in6addr_any;
      std::memcpy(&ep.storage_, &v6, sizeof(v6));
      ep.len_ = sizeof(v6);
      return ep;
    }
    if (::inet_pton(AF_INET6, ip.c_str(), &v6.sin6_addr) == 1) {
      std::memcpy(&ep.storage_, &v6, sizeof(v6));
      ep.len_ = sizeof(v6);
      return ep;
    }

    throw std::runtime_error("invalid ip: " + ip);
  }

  static endpoint from_sockaddr(const sockaddr* sa, socklen_t len) {
    if (sa == nullptr || len == 0 || len > sizeof(sockaddr_storage)) {
      throw std::runtime_error("invalid sockaddr");
    }
    endpoint ep;
    std::memcpy(&ep.storage_, sa, len);
    ep.len_ = len;
    return ep;
  }

  const sockaddr* data() const noexcept {
    return reinterpret_cast<const sockaddr*>(&storage_);
  }

  sockaddr* data() noexcept {
    return reinterpret_cast<sockaddr*>(&storage_);
  }

  socklen_t size() const noexcept { return len_; }
  int family() const noexcept { return data()->sa_family; }

  std::string ip() const {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (family() == AF_INET) {
      auto* a = reinterpret_cast<const sockaddr_in*>(&storage_);
      ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
      return std::string(buf);
    }
    if (family() == AF_INET6) {
      auto* a = reinterpret_cast<const sockaddr_in6*>(&storage_);
      ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
      return std::string(buf);
    }
    return {};
  }

  uint16_t port() const noexcept {
    if (family() == AF_INET) {
      auto* a = reinterpret_cast<const sockaddr_in*>(&storage_);
      return ntohs(a->sin_port);
    }
    if (family() == AF_INET6) {
      auto* a = reinterpret_cast<const sockaddr_in6*>(&storage_);
      return ntohs(a->sin6_port);
    }
    return 0;
  }

  std::string to_string() const {
    if (family() == AF_INET6) {
      return "[" + ip() + "]:" + std::to_string(port());
    }
    return ip() + ":" + std::to_string(port());
  }

 private:
  sockaddr_storage storage_{};
  socklen_t len_{};
};

} // namespace xcoro::net
```

### 9.3 `byte_buffer` + view（用于 I/O）

```cpp
// include/xcoro/net/byte_buffer.hpp
#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace xcoro::net {

struct mutable_buffer {
  std::span<std::byte> bytes;
};

struct const_buffer {
  std::span<const std::byte> bytes;
};

class byte_buffer {
 public:
  explicit byte_buffer(std::size_t initial = 4096)
      : buf_(initial), read_(0), write_(0) {}

  std::size_t size() const noexcept { return write_ - read_; }
  std::size_t capacity() const noexcept { return buf_.size(); }
  std::size_t writable() const noexcept { return buf_.size() - write_; }

  std::span<const std::byte> data() const noexcept {
    return std::span<const std::byte>(buf_.data() + read_, size());
  }

  std::span<std::byte> prepare(std::size_t n) {
    ensure_writable(n);
    return std::span<std::byte>(buf_.data() + write_, n);
  }

  void commit(std::size_t n) noexcept {
    assert(n <= writable());
    write_ += n;
  }

  void consume(std::size_t n) noexcept {
    assert(n <= size());
    read_ += n;
    if (read_ == write_) {
      clear();
    }
  }

  void clear() noexcept {
    read_ = 0;
    write_ = 0;
  }

  void shrink_to_fit() {
    if (size() == 0) {
      std::vector<std::byte>().swap(buf_);
      clear();
      return;
    }
    std::vector<std::byte> next(size());
    std::memcpy(next.data(), buf_.data() + read_, size());
    buf_.swap(next);
    write_ = size();
    read_ = 0;
  }

 private:
  void ensure_writable(std::size_t n) {
    if (writable() >= n) {
      return;
    }
    if (read_ + writable() >= n) {
      const auto s = size();
      std::memmove(buf_.data(), buf_.data() + read_, s);
      read_ = 0;
      write_ = s;
      return;
    }
    buf_.resize(write_ + n);
  }

  std::vector<std::byte> buf_;
  std::size_t read_;
  std::size_t write_;
};

} // namespace xcoro::net
```

### 9.4 `socket`（融合 `tcp_stream`）

```cpp
// include/xcoro/net/socket.hpp
#pragma once

#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/byte_buffer.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
#include "xcoro/task.hpp"

namespace xcoro::net {

class socket {
 public:
  socket() noexcept = default;
  socket(io_context& ctx, int fd) noexcept : ctx_(&ctx), fd_(fd) {}

  static socket open_tcp(io_context& ctx, int family = AF_INET) {
    int fd = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == -1) {
      throw std::system_error(errno, std::system_category(), "socket(TCP) failed");
    }
    return socket{ctx, fd};
  }

  static socket open_udp(io_context& ctx, int family = AF_INET) {
    int fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == -1) {
      throw std::system_error(errno, std::system_category(), "socket(UDP) failed");
    }
    return socket{ctx, fd};
  }

  socket(socket&& other) noexcept : ctx_(other.ctx_), fd_(other.fd_) {
    other.ctx_ = nullptr;
    other.fd_ = -1;
  }

  socket& operator=(socket&& other) noexcept {
    if (this != &other) {
      close();
      ctx_ = other.ctx_;
      fd_ = other.fd_;
      other.ctx_ = nullptr;
      other.fd_ = -1;
    }
    return *this;
  }

  socket(const socket&) = delete;
  socket& operator=(const socket&) = delete;

  ~socket() { close(); }

  void bind(const endpoint& ep) {
    if (::bind(fd_, ep.data(), ep.size()) == -1) {
      throw std::system_error(errno, std::system_category(), "bind failed");
    }
  }

  void listen(int backlog = SOMAXCONN) {
    if (::listen(fd_, backlog) == -1) {
      throw std::system_error(errno, std::system_category(), "listen failed");
    }
  }

  void set_nonblocking(bool on = true) {
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags == -1) {
      throw std::system_error(errno, std::system_category(), "fcntl(F_GETFL) failed");
    }
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd_, F_SETFL, flags) == -1) {
      throw std::system_error(errno, std::system_category(), "fcntl(F_SETFL) failed");
    }
  }

  void set_reuse_addr(bool on = true) {
    int v = on ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == -1) {
      throw std::system_error(errno, std::system_category(), "setsockopt(SO_REUSEADDR) failed");
    }
  }

  void set_reuse_port(bool on = true) {
    int v = on ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &v, sizeof(v)) == -1) {
      throw std::system_error(errno, std::system_category(), "setsockopt(SO_REUSEPORT) failed");
    }
  }

  void set_tcp_nodelay(bool on = true) {
    int v = on ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == -1) {
      throw std::system_error(errno, std::system_category(), "setsockopt(TCP_NODELAY) failed");
    }
  }

  task<> async_connect(const endpoint& ep, cancellation_token token = {}) {
    ensure_context();
    for (;;) {
      throw_if_cancellation_requested(token);
      int r = ::connect(fd_, ep.data(), ep.size());
      if (r == 0) {
        co_return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EINPROGRESS || errno == EALREADY) {
        // 等待 fd 可写并校验 SO_ERROR
        co_await ctx_->async_connection(fd_, token);
        co_return;
      }
      throw std::system_error(errno, std::system_category(), "connect failed");
    }
  }

  task<std::size_t> async_read_some(mutable_buffer dst,
                                    cancellation_token token = {}) {
    ensure_context();
    if (dst.bytes.empty()) {
      co_return 0;
    }
    co_return co_await ctx_->async_read_some(fd_, dst.bytes.data(), dst.bytes.size(), token);
  }

  task<std::size_t> async_read_exact(mutable_buffer dst,
                                     cancellation_token token = {}) {
    ensure_context();
    if (dst.bytes.empty()) {
      co_return 0;
    }
    co_return co_await ctx_->async_read_exact(fd_, dst.bytes.data(), dst.bytes.size(), token);
  }

  task<std::size_t> async_write_some(const_buffer src,
                                     cancellation_token token = {}) {
    ensure_context();
    if (src.bytes.empty()) {
      co_return 0;
    }

    for (;;) {
      throw_if_cancellation_requested(token);
      ssize_t n = ::write(fd_, src.bytes.data(), src.bytes.size());
      if (n > 0) {
        co_return static_cast<std::size_t>(n);
      }
      if (n == 0) {
        co_return 0;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // 当前阶段复用 async_connection 的可写等待语义
        co_await ctx_->async_connection(fd_, token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "write failed");
    }
  }

  task<std::size_t> async_write_all(const_buffer src,
                                    cancellation_token token = {}) {
    ensure_context();
    if (src.bytes.empty()) {
      co_return 0;
    }
    co_return co_await ctx_->async_write_all(fd_, src.bytes.data(), src.bytes.size(), token);
  }

  endpoint local_endpoint() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &len) == -1) {
      throw std::system_error(errno, std::system_category(), "getsockname failed");
    }
    return endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss), len);
  }

  endpoint peer_endpoint() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &len) == -1) {
      throw std::system_error(errno, std::system_category(), "getpeername failed");
    }
    return endpoint::from_sockaddr(reinterpret_cast<sockaddr*>(&ss), len);
  }

  void shutdown(int how = SHUT_RDWR) {
    if (::shutdown(fd_, how) == -1) {
      throw std::system_error(errno, std::system_category(), "shutdown failed");
    }
  }

  int native_handle() const noexcept { return fd_; }
  bool is_open() const noexcept { return fd_ != -1; }

  void close() noexcept {
    if (fd_ != -1) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  void ensure_context() const {
    if (!ctx_) {
      throw std::runtime_error("socket is not bound to io_context");
    }
  }

  io_context* ctx_{nullptr};
  int fd_{-1};
};

} // namespace xcoro::net
```

### 9.5 `acceptor`（替代 `tcp_acceptor`）

```cpp
// include/xcoro/net/acceptor.hpp
#pragma once

#include <cerrno>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <utility>

#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
#include "xcoro/net/socket.hpp"
#include "xcoro/task.hpp"

namespace xcoro::net {

class acceptor {
 public:
  acceptor() = delete;
  acceptor(io_context& ctx, socket listen_sock)
      : ctx_(&ctx), sock_(std::move(listen_sock)) {}

  static acceptor bind(io_context& ctx, const endpoint& ep, int backlog = SOMAXCONN) {
    auto s = socket::open_tcp(ctx, ep.family());
    s.set_reuse_addr(true);
    s.set_reuse_port(true);
    s.set_nonblocking(true);
    s.bind(ep);
    s.listen(backlog);
    return acceptor{ctx, std::move(s)};
  }

  socket& native_socket() noexcept { return sock_; }
  const socket& native_socket() const noexcept { return sock_; }

  task<socket> async_accept(cancellation_token token = {}) {
    for (;;) {
      throw_if_cancellation_requested(token);

      sockaddr_storage ss{};
      socklen_t len = sizeof(ss);
      int fd = ::accept4(sock_.native_handle(),
                         reinterpret_cast<sockaddr*>(&ss),
                         &len,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd >= 0) {
        co_return socket{ctx(), fd};
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        co_await ctx().async_accept(sock_.native_handle(), token);
        continue;
      }
      throw std::system_error(errno, std::system_category(), "accept failed");
    }
  }

 private:
  io_context& ctx() {
    if (!ctx_) {
      throw std::runtime_error("acceptor has no io_context");
    }
    return *ctx_;
  }

  io_context* ctx_{nullptr};
  socket sock_;
};

} // namespace xcoro::net
```

### 9.6 `resolver`（同步 + 协程异步）

```cpp
// include/xcoro/net/resolver.hpp
#pragma once

#include <netdb.h>

#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
#include "xcoro/task.hpp"

namespace xcoro::net {

struct resolve_options {
  int family = AF_UNSPEC;
  int socktype = SOCK_STREAM;
  int flags = AI_ADDRCONFIG;
};

class resolver {
 public:
  static std::vector<endpoint> resolve(std::string_view host,
                                       std::string_view service,
                                       resolve_options opt = {}) {
    addrinfo hints{};
    hints.ai_family = opt.family;
    hints.ai_socktype = opt.socktype;
    hints.ai_flags = opt.flags;

    std::string host_str(host);
    if (!host_str.empty() && host_str.front() == '[' && host_str.back() == ']') {
      host_str = host_str.substr(1, host_str.size() - 2);
    }

    addrinfo* res = nullptr;
    const char* host_ptr = host_str.empty() ? nullptr : host_str.c_str();
    std::string service_str(service);
    int rc = ::getaddrinfo(host_ptr, service_str.c_str(), &hints, &res);
    if (rc != 0) {
      throw std::runtime_error(::gai_strerror(rc));
    }

    std::vector<endpoint> out;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
      out.push_back(endpoint::from_sockaddr(p->ai_addr, p->ai_addrlen));
    }
    ::freeaddrinfo(res);
    return out;
  }

  static task<std::vector<endpoint>> async_resolve(io_context& ctx,
                                                   std::string host,
                                                   std::string service,
                                                   resolve_options opt = {},
                                                   cancellation_token token = {}) {
    throw_if_cancellation_requested(token);

    // getaddrinfo 是阻塞调用，先投递到后台线程
    auto fut = std::async(std::launch::async, [host = std::move(host),
                                               service = std::move(service),
                                               opt]() mutable {
      return resolve(host, service, opt);
    });

    // 协作式等待 future，同时让调用方可取消
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      throw_if_cancellation_requested(token);
      co_await ctx.sleep_for(std::chrono::milliseconds(1), token);
    }
    co_return fut.get();
  }
};

} // namespace xcoro::net
```

### 9.7 取消路径检查清单（你写代码时逐项对）

1. `async_*` 循环里，进入阻塞点前都调用 `throw_if_cancellation_requested(token)`。
2. `io_awaiter`/`timer_awaiter` 只允许一次 resume（`completed_` CAS/atomic）。
3. 取消回调里不抛异常；只做标记 + 解除监听 + 唤醒。
4. `await_resume()` 统一把取消转换成 `operation_cancelled`。
5. `when_all/when_any` 用共享 token 才能把网络操作协同取消。
