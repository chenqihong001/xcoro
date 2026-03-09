# 网络模块重构设计（xcoro）

本文档是网络层的重写目标说明。  
目标是“借鉴 Asio 的 reactor + endpoint 思路”，但接口全部走现代 C++ 协程风格，不保留 callback 历史包袱。

本文涉及的网络组件：

- `io_context`（事件循环，当前代码对应 `io_scheduler`）
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

这与当前 `io_scheduler::io_awaiter` / `timer_awaiter` 的实现一致，后续网络组件都复用同一语义。

---

## 4. 组件设计

### 4.1 io_context（当前 `io_scheduler`）

建议把 `io_scheduler` 语义提升为 `io_context` 命名，保留别名兼容：

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
};

using io_scheduler = io_context; // 迁移期别名
} // namespace xcoro::net
```

实现继续沿用 epoll + eventfd + ready queue + timer heap，不需要回调接口。
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
- `io_scheduler` -> `io_context`（可保留 type alias 兼容）。

---

## 8. 落地顺序建议

1. 先做命名层兼容：`io_context` / `acceptor` 别名与包装。
2. 合并 `tcp_stream` 到 `socket`，保持底层 `io_scheduler` 不变。
3. 引入 `endpoint` 值类型并逐步替换 `shared_ptr<address>`。
4. 引入新 `byte_buffer`，旧 `buffer` 暂时保留兼容适配层。
5. 最后清理旧 API（`tcp_*` 与模糊 read/write 别名）。

---

## 9. 参考实现代码（完整）

这一节给的是完整参考实现片段（header-only 风格）。  
你可以按这里的文件拆分写进项目，再按自己的命名细节调整。

### 9.1 `io_context`（直接复用现有 `io_scheduler`）

```cpp
// include/xcoro/net/io_context.hpp
#pragma once

#include "xcoro/net/io_scheduler.hpp"

namespace xcoro::net {

// 现阶段先别名复用，避免维护两套 reactor 代码
using io_context = io_scheduler;

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
        // 复用现有 io_scheduler 的 connect-completion 等待
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
