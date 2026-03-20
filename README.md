# xcoro

`xcoro` 是一个基于 C++20 协程的轻量头文件库，提供：

- 基础协程类型：`task<T>`、`generator<T>`
- 组合原语：`sync_wait`、`when_all`、`when_any`
- 取消模型：`cancellation_source`、`cancellation_token`、`operation_cancelled`
- 同步原语：`manual_reset_event`、`semaphore`、`mutex`、`condition_variable`
- 调度组件：`thread_pool`
- Linux 网络 I/O：`net::io_context`、`socket`、`acceptor`、`resolver`

它的目标不是做一个“大而全”的异步框架，而是提供一组结构清晰、容易组合、容易继续扩展的协程基础组件。

## 适合用来做什么

如果你的项目需要下面这些能力，`xcoro` 比较适合：

- 用 `task<T>` 把异步逻辑写成顺序代码
- 在多个协程之间做并发等待和结果聚合
- 给等待操作增加取消能力
- 在不引入大型运行时的前提下使用基础同步原语
- 在 Linux 上用 `epoll + eventfd` 跑简单网络协程
- 用一个轻量线程池把协程切到后台线程执行

## 环境要求

- C++20 编译器
  建议 `GCC 13+` 或同等级 `Clang`
- CMake `3.16+`
- `pthread`
- Linux
  `net` 模块依赖 `epoll` / `eventfd`

说明：

- 非网络模块基本都是头文件实现
- `net` 模块当前明确按 Linux 语义设计
- `thread_pool` 已可使用，但仍建议视为实验接口

## 接入库

当前仓库已经导出标准的 header-only CMake target：

- package name: `xcoro`
- target name: `xcoro::xcoro`

推荐始终通过 `target_link_libraries(... xcoro::xcoro)` 来接入。  
这样 C++20、头文件目录和 `Threads::Threads` 依赖都会自动带上。

### 方式 1：安装后使用 `find_package`

先在 `xcoro` 仓库根目录安装：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXCORO_BUILD_TESTS=OFF
cmake --build build --parallel
cmake --install build --prefix /path/to/xcoro-install
```

然后在你的项目里这样写：

```cmake
find_package(xcoro CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE xcoro::xcoro)
```

配置你的项目时，把安装前缀传给 CMake：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/xcoro-install
```

### 方式 2：通过 `FetchContent` 直接拉源码

```cmake
include(FetchContent)

set(XCORO_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  xcoro
  GIT_REPOSITORY https://github.com/<your-org>/xcoro.git
  GIT_TAG main
)

FetchContent_MakeAvailable(xcoro)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE xcoro::xcoro)
```

建议：

- 如果你是正式项目，最好把 `GIT_TAG` 固定为具体 tag 或 commit
- 如果你只是本地联调，先跟 `main` 也可以

### 方式 3：作为子目录引入

```cmake
set(XCORO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/xcoro)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE xcoro::xcoro)
```

### 方式 4：不使用 CMake，直接包含头文件

如果你只是快速试验，也可以直接手工编译：

```bash
g++ -std=c++20 -O2 -pthread -I./include demo.cpp -o demo
./demo
```

这种方式最直接，但不适合长期维护。  
一旦项目规模上来，还是推荐用 `xcoro::xcoro` 这个 target。

## 第一个程序

### 1. `task + sync_wait`

这是最基础的使用方式：定义一个返回 `task<T>` 的协程，然后用 `sync_wait()` 在当前线程里等待结果。

```cpp
#include <iostream>

#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

xcoro::task<int> twice(int x) {
  co_return x * 2;
}

int main() {
  const int value = xcoro::sync_wait(twice(21));
  std::cout << value << "\n";
  return 0;
}
```

适合场景：

- 写最小示例
- 在同步 `main()` 里等待一个协程结果
- 给已有同步代码逐步引入协程

### 2. 并发等待多个任务：`when_all`

`when_all()` 会等待所有 awaitable 完成，并返回聚合结果。

```cpp
#include <string>
#include <tuple>

#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

xcoro::task<int> first() { co_return 3; }
xcoro::task<std::string> second() { co_return "ok"; }

int main() {
  auto result = xcoro::sync_wait(xcoro::when_all(first(), second()));
  return (std::get<0>(result) == 3 && std::get<1>(result) == "ok") ? 0 : 1;
}
```

适合场景：

- 并发发起多个协程，再统一收结果
- 等一组操作全部完成后再继续

### 3. 等最先完成的任务：`when_any`

`when_any()` 会在第一个 awaitable 完成时返回。

```cpp
#include "xcoro/manual_reset_event.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_any.hpp"

xcoro::task<int> fast() {
  co_return 7;
}

xcoro::task<int> slow(xcoro::manual_reset_event& gate) {
  co_await gate;
  co_return 42;
}

int main() {
  xcoro::manual_reset_event gate;
  auto result = xcoro::sync_wait(xcoro::when_any(fast(), slow(gate)));
  return (result.active_index() == 0 && result.get<0>() == 7) ? 0 : 1;
}
```

适合场景：

- 超时竞争
- 多路请求抢最快结果
- “谁先完成就先用谁”的聚合逻辑

### 4. 取消等待：`cancellation_token`

```cpp
#include "xcoro/cancellation_source.hpp"
#include "xcoro/semaphore.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

xcoro::task<void> wait_or_cancel(
    xcoro::semaphore& sem,
    xcoro::cancellation_token token) {
  co_await sem.acquire(token);
}

int main() {
  xcoro::semaphore sem(0);
  xcoro::cancellation_source source;

  try {
    source.request_cancellation();
    xcoro::sync_wait(wait_or_cancel(sem, source.token()));
  } catch (const xcoro::operation_cancelled&) {
  }
}
```

使用模式通常是：

1. 创建 `cancellation_source`
2. 把 `source.token()` 传进协程
3. 在外部调用 `request_cancellation()`
4. 被取消的 await 点抛出 `operation_cancelled`

### 5. 把协程切到线程池：`thread_pool`

```cpp
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/thread_pool.hpp"

xcoro::task<int> compute(xcoro::thread_pool& pool) {
  co_await pool.schedule();
  co_return 42;
}

int main() {
  xcoro::thread_pool pool(4);
  return xcoro::sync_wait(compute(pool)) == 42 ? 0 : 1;
}
```

你可以把它理解成：

- `schedule()`：把当前协程投递到线程池执行
- `yield()`：把当前协程重新排队，让别的任务先跑


### 6. 使用 `io_context` 跑定时/网络协程

`net::io_context` 不只是 I/O reactor，也负责定时器和协程恢复。  
如果你要使用 `net` 模块，最重要的步骤是：

1. 创建 `io_context`
2. 调用 `run()` 或 `run_in_current_thread()`
3. 在协程里使用 `sleep_for()` / `socket` / `acceptor` / `resolver`
4. 结束前调用 `stop()`

下面是一个最小定时示例：

```cpp
#include <chrono>
#include <cstdio>

#include "xcoro/net/io_context.hpp"
#include "xcoro/sync_wait.hpp"

xcoro::task<void> say_later(xcoro::net::io_context& ctx) {
  using namespace std::chrono_literals;
  co_await ctx.sleep_for(50ms);
  std::puts("tick");
}

int main() {
  xcoro::net::io_context ctx;
  ctx.run();
  xcoro::sync_wait(say_later(ctx));
  ctx.stop();
}
```

## 常用头文件

最常见的入口头文件如下：

- `xcoro/task.hpp`
- `xcoro/sync_wait.hpp`
- `xcoro/when_all.hpp`
- `xcoro/when_any.hpp`
- `xcoro/cancellation_source.hpp`
- `xcoro/cancellation_token.hpp`
- `xcoro/manual_reset_event.hpp`
- `xcoro/semaphore.hpp`
- `xcoro/mutex.hpp`
- `xcoro/condition_variable.hpp`
- `xcoro/generator.hpp`
- `xcoro/thread_pool.hpp`
- `xcoro/net/io_context.hpp`
- `xcoro/net/socket.hpp`
- `xcoro/net/acceptor.hpp`
- `xcoro/net/resolver.hpp`
- `xcoro/net/endpoint.hpp`
- `xcoro/net/buffer.hpp`

