# io_scheduler 设计与实现

本文给出 `io_scheduler` 的专业化设计说明，以及一份可落地的参考实现（Linux/epoll 版本）。实现风格与仓库现有 `task` 协程模型兼容，重点是 **清晰的语义、稳定的线程模型、正确的 I/O 等待与定时器处理**。

---

## 设计目标

1. **协程友好**：使用 `co_await` 表达异步 I/O、定时等待与调度让出。
2. **单线程事件循环**：所有回调在事件循环线程恢复执行，避免锁竞争与复杂同步。
3. **高性能 I/O 复用**：基于 `epoll`，支持 `ONESHOT`，避免惊群。
4. **非阻塞语义明确**：读写操作在 `EAGAIN` 时挂起，在可读/可写后继续。
5. **定时器与唤醒**：通过小根堆管理定时器，`eventfd` 唤醒事件循环。

---

## 总体架构

- **事件循环线程**：`io_scheduler::run()` 启动 `std::jthread`，专职运行 `epoll_wait`。
- **就绪队列**：`schedule()` 或 I/O 事件都会把协程句柄压入队列，事件循环批量恢复。
- **定时器堆**：以 `deadline` 排序的小根堆，事件循环计算 `epoll_wait` 超时时间。
- **唤醒机制**：使用 `eventfd` 将跨线程的投递/停止请求转成可读事件。

---

## 核心接口语义

- `run()` / `run_in_current_thread()`：启动事件循环（后台线程 or 当前线程阻塞）。
- `stop()`：停止调度并唤醒事件循环。
- `schedule()`：让出执行权，把当前协程切换到事件循环线程执行。
- `spawn(task<>)`：火并（fire-and-forget），把任务投递到事件循环线程执行。
- `sleep_for()`：按指定时长挂起协程，时间到恢复。
- `async_accept()` / `async_connect()`：封装 accept/connect 等待流程。
- `async_read_some` / `async_read_exact` / `async_write_all`：提供明确的 I/O 语义。
- `... + cancellation_token`：所有等待类 API 均提供取消版本。

> 兼容旧接口：`async_read/async_write` 仍可用，语义等价于 `async_read_some/async_write_all`。

---

## I/O 语义说明

- `async_read_some`：尽快读取 **至少 1 字节** 后返回；若对端关闭则返回 0。
- `async_read_exact`：尝试读取 **固定字节数**；若中途 EOF，则返回已读字节数（可能小于期望）。
- `async_write_all`：保证把指定数据 **全部写出**，否则抛异常。

所有 I/O 都假设 fd 为 **非阻塞**，在 `EAGAIN/EWOULDBLOCK` 时挂起协程。

---

## 取消支持

`io_scheduler` 支持 cppcoro 风格取消：

```cpp
xcoro::cancellation_source src;
auto token = src.token();

co_await sched.sleep_for(1s, token);
co_await sched.async_read_some(fd, buf, len, token);
```

取消触发时：
- 立即恢复等待协程并抛出 `xcoro::operation_cancelled`
- I/O 等待会从 epoll 中移除，避免后续误唤醒

---

## spawn 与调度

- `spawn(task<>)` 是 fire-and-forget：它会把协程切到事件循环线程执行。
- `spawn` 内部会先 `co_await schedule()`，所以 **scheduler 必须已运行**（`run()` 或 `run_in_current_thread()`）。

---

## 事件循环与调度流程

1. 计算 **下一次定时器到期时间**，作为 `epoll_wait` 的超时参数。
2. `epoll_wait` 返回后：
   - 如果是 `eventfd` 事件，清空计数并继续；
   - 如果是 I/O 事件，找到对应的 `io_awaiter`，把协程句柄放入就绪队列；
3. 处理已到期的定时器，把对应协程句柄放入就绪队列；
4. 批量恢复就绪队列中的协程。

---

## I/O awaiter 设计

`io_awaiter` 只负责 **等待 fd 就绪**，不直接执行读写。实际读写在 `async_read_* / async_write_*` 中完成。  
这样设计有几个好处：

- 可以正确处理 **spurious wakeup**（即事件就绪但读写仍返回 `EAGAIN`）；
- 可在 I/O 协程内循环，直到成功或发生不可恢复错误；
- 逻辑更直观：I/O 逻辑与等待逻辑分离。

---

## 定时器设计

- 使用 **小根堆** 保存 `(deadline, coroutine_handle)`；
- 事件循环在每次 `epoll_wait` 前计算超时时间；
- `sleep_for()` 只是把当前协程放入定时器堆并唤醒事件循环。

---

## 线程安全与停止行为

- `schedule()`、`sleep_for()`、`spawn()` 允许跨线程调用；
- 投递协程时只持有短暂锁并用 `eventfd` 唤醒；
- `stop()` 设置原子标志并唤醒事件循环，安全退出；
- `run_in_current_thread()` 适合单线程调试或嵌入式使用。

---

## 实现位置

`io_scheduler` 为 **header-only** 实现，完整代码位于：

- `include/xcoroutine/net/io_scheduler.hpp`

相关网络组件说明见：`docs/net.md`

---

## 使用示例

```cpp
xcoro::net::io_scheduler sched;
sched.run();

auto echo = [&]() -> xcoro::task<> {
  char buf[1024];
  for (;;) {
    auto n = co_await sched.async_read_some(client_fd, buf, sizeof(buf));
    if (n == 0) {
      co_return;
    }
    co_await sched.async_write_all(client_fd, buf, n);
  }
};

sched.spawn(echo());
```

### 与 socket/buffer/address 搭配

```cpp
using namespace xcoro::net;

io_scheduler sched;
sched.run();

auto server = socket::create_tcp();
server.set_reuse_addr(true);
server.set_reuse_port(true);
server.bind(*address::from_string("0.0.0.0", 9000));
server.listen();
server.set_nonblocking(true);

auto accept_loop = [&]() -> xcoro::task<> {
  for (;;) {
    auto [client, peer] = co_await sched.async_accept(server);
    (void)peer;

    buffer buf;
    for (;;) {
      auto n = co_await sched.async_read_some(client, buf, 4096);
      if (n == 0) {
        co_return;
      }
      co_await sched.async_write_all(client, buf);
    }
  }
};

sched.spawn(accept_loop());
```

---

## 可选增强方向

1. **多线程事件循环**：将 io_scheduler 扩展为多 reactor/多线程模型。
2. **批量唤醒与限流**：避免单次事件处理过多协程导致长尾延迟。
3. **完善错误分类**：把 `EPIPE/ECONNRESET` 等常见错误转化为更友好的异常类型。
