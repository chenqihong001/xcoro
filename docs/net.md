# 网络组件说明

本文档覆盖网络模块的常用组件及推荐用法：

- `address`：网络地址抽象（IPv4/IPv6）
- `socket`：系统 socket 的 RAII 封装
- `buffer`：可增长缓冲区
- `resolver`：域名解析与多地址解析
- `tcp_stream`：已连接 TCP 流
- `tcp_acceptor`：监听/accept 封装
- `io_scheduler`：协程 I/O 调度器（详见 `docs/io_scheduler.md`）

---

## address

`address` 为虚基类，具体实现为：

- `ipv4_address`
- `ipv6_address`

### 解析与格式化

```cpp
auto a = xcoro::net::address::from_string("127.0.0.1", 8080);
auto b = xcoro::net::address::from_string("[::1]", 8080);

std::cout << a->to_string(); // 127.0.0.1:8080
std::cout << b->to_string(); // [::1]:8080
```

说明：
- IPv6 的 `to_string()` 输出 `[...]` 包裹，方便拼接端口。
- `from_string` 支持 `"[::1]"` 形式。

---

## socket

`socket` 负责：
- 创建/关闭 socket（RAII）
- 设置选项（复用端口、非阻塞、Nagle 等）
- 绑定、监听、accept、连接

### 创建

```cpp
auto s = xcoro::net::socket::create_tcp();   // IPv4 TCP
auto u = xcoro::net::socket::create_udp();   // IPv4 UDP
```

### 常见设置

```cpp
s.set_reuse_addr(true);
s.set_reuse_port(true);
s.set_tcp_nodelay(true);
s.set_nonblocking(true);
```

### 阻塞 / 非阻塞 connect

```cpp
// 阻塞（会重试 EINTR）
s.connect_blocking(*addr);

// 非阻塞：返回 true 表示立即成功，false 表示正在连接
bool ok = s.connect_nonblocking(*addr);
```

---

## buffer

`buffer` 是可增长缓冲区，常用于 socket 读写：

```cpp
xcoro::net::buffer buf;
buf.append("hello");
buf.append(std::string_view{" world"});

auto view = buf.readable_view();   // std::string_view
auto span = buf.readable_span();   // std::span<const char>
```

要点：
- `ensure_writable()` 保证可写空间；
- `has_written()` / `has_read()` 控制读写指针；
- 支持 `append(std::string_view)`，减少拷贝。

---

## resolver

`resolver` 基于 `getaddrinfo`，用于域名解析：

```cpp
auto addrs = xcoro::net::resolver::resolve("example.com", 80);
for (auto& addr : addrs) {
  std::cout << addr->to_string() << "\n";
}
```

支持设置：
- `family`（`AF_UNSPEC` / `AF_INET` / `AF_INET6`）
- `socktype`（`SOCK_STREAM` / `SOCK_DGRAM`）
- `flags`（默认 `AI_ADDRCONFIG`）

实践建议：按返回顺序尝试连接，直到成功或全部失败。

---

## tcp_stream

`tcp_stream` 表示已连接的 TCP 流，绑定 `io_scheduler` 后可直接调用协程 I/O：

```cpp
xcoro::net::tcp_stream stream(sched, std::move(sock));
xcoro::net::buffer buf;

co_await stream.async_read_some(buf, 4096);
co_await stream.async_write_all(buf);
```

> 注意：`tcp_stream` 必须绑定调度器（构造时传入或调用 `set_scheduler`）。
> 所有 read/write 接口均提供带 `cancellation_token` 的重载版本。

---

## tcp_acceptor

监听/accept 的高层封装：

```cpp
auto acceptor = xcoro::net::tcp_acceptor::listen(
    sched, *xcoro::net::address::from_string("0.0.0.0", 9000));

auto stream = co_await acceptor.accept();
```

`tcp_acceptor` 会自动设置：
- `SO_REUSEADDR`
- `SO_REUSEPORT`
- `O_NONBLOCK`

`accept()` 也支持 `accept(token)` 版本，用于可取消等待。

---

## 组合示例（Echo Server）

```cpp
xcoro::net::io_scheduler sched;
sched.run();

auto acceptor = xcoro::net::tcp_acceptor::listen(
    sched, *xcoro::net::address::from_string("127.0.0.1", 9000));

auto echo = [&]() -> xcoro::task<> {
  auto stream = co_await acceptor.accept();
  xcoro::net::buffer buf;
  for (;;) {
    auto n = co_await stream.async_read_some(buf, 4096);
    if (n == 0) {
      co_return;
    }
    co_await stream.async_write_all(buf);
  }
};

sched.spawn(echo());
```

---

## 建议的使用习惯

- **非阻塞优先**：socket 一律 `set_nonblocking(true)`，由 `io_scheduler` 驱动。
- **语义明确**：用 `async_read_some/async_read_exact/async_write_all`，不要用模糊接口。
- **调度器先运行**：调用 `spawn()` 前先 `run()` 或 `run_in_current_thread()`。
