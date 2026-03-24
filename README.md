# xcoro C++ coroutine library

## Overview
`xcoro` 是一个基于 C++20 coroutine 的轻量级协程库，提供了常见的协程任务抽象、同步原语、调度器、取消机制，以及基于 `epoll` 的网络事件循环能力。

* C++20 coroutine
* Header-only library
* Modern Safe C++20 API
* 协程工具
  - [xcoro::task<T>](#task)
  - [xcoro::sync_wait(awaitable)](#sync_wait)
  - [xcoro::when_all(awaitable...)](#when_all)
  - [xcoro::when_any(awaitable...)](#when_any)
  - [xcoro::generator<T>](#generator)
  - [xcoro::manual_reset_event](#manual_reset_event)
  - [xcoro::mutex](#mutex)
  - [xcoro::condition_variable](#condition_variable)
  - [xcoro::semaphore](#semaphore)
* 调度器
  - [xcoro::thread_pool](#thread_pool)
  - [xcoro::net::io_context](#io_context)
* 网络
  - [xcoro::net::socket](#socket)
  - [xcoro::net::acceptor](#acceptor)
  - [xcoro::net::resolver](#resolver)
* 取消机制
  - [xcoro::cancellation_source](#cancellation_source)
  - [xcoro::cancellation_token](#cancellation_token)

## Build
`xcoro` 是纯头文件库。对外使用时只需要把仓库里的 `include/` 目录加入头文件搜索路径即可，不再提供 `find_package()`、安装导出或 CMake package 配置。

```bash
g++ -std=c++20 -pthread -I/path/to/xcoro/include demo.cpp -o demo
```

如果你自己的项目也用 CMake，最简单的接入方式就是直接加头文件路径：

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

target_include_directories(your_target PRIVATE /path/to/xcoro/include)
target_compile_options(your_target PRIVATE -pthread)
target_link_options(your_target PRIVATE -pthread)
```

仓库根目录下的 `CMakeLists.txt` 现在只用于构建本仓库测试，不再生成安装包或导出 target。

如果你要跑仓库自带测试，可以使用：

```bash
cmake -S . -B build -DXCORO_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

网络部分当前基于 `epoll/eventfd` 实现，主要面向 Linux 环境；涉及线程池、同步原语或网络模块时，编译时建议带上 `-pthread`。

## Usage
### task
`task<T>` 是 `xcoro` 里最核心的抽象之一，它表示一个`惰性异步任务`。调用协程函数时并不会立刻执行函数体，而是先得到一个 `task<T>` 对象；只有在它被 `co_await` 或 `sync_wait()` 驱动时，协程才真正开始运行。

```cpp
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

int main() {
  auto add = [](int a, int b) -> xcoro::task<int> {
    co_return a + b;
  };

  auto calc = [&]() -> xcoro::task<int> {
    int lhs = 100;
    int rhs = 100;
    co_return co_await add(lhs, rhs);
  };

  int first = xcoro::sync_wait(add(10, 20));
  int second = xcoro::sync_wait(calc());
  return (first == 30 && second == 200) ? 0 : 1;
}
```

### sync_wait
`sync_wait()` 用来把协程世界和普通同步代码连接起来。它会在当前线程里阻塞等待某个 awaitable 完成，并返回结果或重新抛出异常。

```cpp
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

int main() {
  auto job = []() -> xcoro::task<> {
    std::this_thread::sleep_for(2s);
    std::cout << "job end\n";
    co_return;
  };

  xcoro::sync_wait(job());
  std::cout << "main end\n";
  return 0;
}
```

最终输出顺序会是先输出 `job end`，然后 `sync_wait()` 返回，最后输出 `main end`。

### when_all
`when_all()` 用于并发等待多个 awaitable 全部完成。它会同时启动所有任务，并在全部完成后一次性返回结果。返回值是一个 `std::tuple`，每个元素对应一个输入 awaitable 的结果；如果某个 awaitable 返回 `void`，对应元素类型会是 `xcoro::void_value`。

```cpp
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

int main() {
  auto first = []() -> xcoro::task<int> {
    co_return 3;
  };

  auto second = []() -> xcoro::task<std::string> {
    std::this_thread::sleep_for(1s);
    co_return std::string{"ok"};
  };

  auto [number, text] = xcoro::sync_wait(xcoro::when_all(first(), second()));
  return (number == 3 && text == "ok") ? 0 : 1;
}
```

### when_any
`when_any()` 用于等待多个 awaitable 中任意一个先完成。它会返回一个 `when_any_result`，可以通过 `active_index()`、`holds<Index>()` 和 `get<Index>()` 判断是谁先完成，并取出对应结果。

```cpp
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_any.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

int main() {
  auto slow = []() -> xcoro::task<int> {
    std::this_thread::sleep_for(2s);
    co_return 100;
  };

  auto fast = []() -> xcoro::task<std::string> {
    std::this_thread::sleep_for(1s);
    co_return std::string{"hello"};
  };

  auto result = xcoro::sync_wait(xcoro::when_any(slow(), fast()));
  if (result.holds<1>()) {
    return result.get<1>() == "hello" ? 0 : 1;
  }
  return 1;
}
```

### generator
`generator<T>` 用于实现`惰性序列生成`。它通过 `co_yield` 逐个产出元素，而不是一次性构造整个容器，适合表示流式数据、遍历器或无限序列。

```cpp
#include "xcoro/generator.hpp"

#include <vector>

int main() {
  auto fibonacci = [](int limit) -> xcoro::generator<long long> {
    long long a = 0;
    long long b = 1;
    for (int i = 0; i < limit; ++i) {
      co_yield a;
      long long next = a + b;
      a = b;
      b = next;
    }
  };

  std::vector<long long> fib_nums;
  for (auto num : fibonacci(10)) {
    fib_nums.push_back(num);
  }

  return fib_nums.size() == 10 ? 0 : 1;
}
```

### manual_reset_event
`manual_reset_event` 是一个手动触发的事件同步原语，允许一个或多个协程等待同一个事件。一旦调用 `set()`，所有已经等待的协程都会被恢复，后续新的等待者也会直接通过而不再挂起。

```cpp
#include "xcoro/manual_reset_event.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

#include <cassert>

int main() {
  bool ready = false;
  xcoro::manual_reset_event ev;

  auto waiter = [&]() -> xcoro::task<> {
    co_await ev;
    assert(ready);
    co_return;
  };

  auto signaler = [&]() -> xcoro::task<> {
    ready = true;
    ev.set();
    co_return;
  };

  xcoro::sync_wait(xcoro::when_all(waiter(), signaler()));
  return 0;
}
```

### mutex
`xcoro::mutex` 是协程友好的互斥锁。它和传统 `std::mutex` 的最大区别是：当锁不可用时，它会挂起当前协程，而不是阻塞整个线程。

```cpp
#include "xcoro/mutex.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/thread_pool.hpp"
#include "xcoro/when_all.hpp"

#include <cassert>

int main() {
  xcoro::thread_pool pool(2);
  xcoro::mutex mtx;
  int shared = 0;

  auto worker = [&]() -> xcoro::task<> {
    co_await pool.schedule();
    co_await mtx.lock();
    ++shared;
    mtx.unlock();
    co_return;
  };

  xcoro::sync_wait(xcoro::when_all(worker(), worker()));
  assert(shared == 2);
  return 0;
}
```

### condition_variable
`xcoro::condition_variable` 用来配合 `xcoro::mutex` 实现条件等待。`co_await cv.wait(mtx)` 会先把当前协程加入等待队列，再释放互斥锁；被 `notify_one()` 或 `notify_all()` 唤醒时，会先重新获得互斥锁，再从 `co_await` 返回。

```cpp
#include "xcoro/condition_variable.hpp"
#include "xcoro/mutex.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

int main() {
  xcoro::mutex mtx;
  xcoro::condition_variable cv;
  bool ready = false;

  auto waiter = [&]() -> xcoro::task<> {
    co_await mtx.lock();
    while (!ready) {
      co_await cv.wait(mtx);
    }
    mtx.unlock();
    co_return;
  };

  auto notifier = [&]() -> xcoro::task<> {
    co_await mtx.lock();
    ready = true;
    mtx.unlock();
    cv.notify_one();
    co_return;
  };

  xcoro::sync_wait(xcoro::when_all(waiter(), notifier()));
  return 0;
}
```

### semaphore
`xcoro::semaphore` 是一个计数信号量，适合控制并发度或表示“可用许可数”。`co_await sem.acquire()` 在有许可时会立即继续，没有许可时就挂起；`release()` 会释放一个或多个许可并恢复等待者。库里还提供了 `xcoro::binary_semaphore` 作为单许可别名。

```cpp
#include "xcoro/semaphore.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

#include <cassert>

int main() {
  xcoro::semaphore sem(0);
  bool acquired = false;

  auto waiter = [&]() -> xcoro::task<> {
    co_await sem.acquire();
    acquired = true;
    co_return;
  };

  auto releaser = [&]() -> xcoro::task<> {
    sem.release();
    co_return;
  };

  xcoro::sync_wait(xcoro::when_all(waiter(), releaser()));
  assert(acquired);
  return 0;
}
```

### thread_pool
`thread_pool` 提供了一个协程调度器，可以把协程恢复到线程池中的工作线程上执行。`co_await pool.schedule()` 用来把当前协程投递给线程池；`co_await pool.yield()` 则表示当前协程主动让出执行机会，稍后再重新排队运行。

```cpp
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/thread_pool.hpp"

#include <cassert>
#include <thread>

int main() {
  xcoro::thread_pool pool(2);
  const std::thread::id caller = std::this_thread::get_id();

  const std::thread::id resumed =
      xcoro::sync_wait([&]() -> xcoro::task<std::thread::id> {
        co_await pool.schedule();
        co_return std::this_thread::get_id();
      }());

  assert(resumed != caller);
  return 0;
}
```

### io_context
`xcoro::net::io_context` 是网络和定时器相关 awaitable 的核心事件循环。它内部负责 `epoll`、ready queue 和 timer queue 的调度，支持异步读写、连接、接受连接、DNS 解析和 `sleep_for()`。

```cpp
#include "xcoro/net/io_context.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"

#include <chrono>

using namespace std::chrono_literals;

int main() {
  xcoro::net::io_context ctx;
  ctx.run();

  xcoro::sync_wait([&]() -> xcoro::task<> {
    co_await ctx.schedule();
    co_await ctx.sleep_for(50ms);
    co_return;
  }());

  ctx.stop();
  return 0;
}
```

如果你不想单独启动后台线程，也可以使用 `run_in_current_thread()` 在当前线程直接跑事件循环。

### socket
`xcoro::net::socket` 是对非阻塞 socket 的 RAII 封装，提供了 `async_connect()`、`async_read_some()`、`async_read_exact()`、`async_write_some()`、`async_write_all()` 等协程接口。读写接口使用 `xcoro::net::mutable_buffer` / `xcoro::net::const_buffer`，更复杂的收发场景可以配合 `xcoro::net::byte_buffer` 一起使用。

```cpp
#include "xcoro/net/io_context.hpp"
#include "xcoro/net/socket.hpp"
#include "xcoro/sync_wait.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <span>
#include <sys/socket.h>

int main() {
  int fds[2] = {-1, -1};
  int rc = ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds);
  assert(rc == 0);

  xcoro::net::io_context ctx;
  ctx.run();

  xcoro::net::socket left(ctx, fds[0]);
  xcoro::net::socket right(ctx, fds[1]);

  std::array<std::byte, 5> out{
      std::byte{'h'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};
  std::array<std::byte, 5> in{};

  xcoro::sync_wait(left.async_write_all({std::span<const std::byte>{out}}));
  xcoro::sync_wait(right.async_read_exact({std::span<std::byte>{in}}));

  ctx.stop();
  return 0;
}
```

### acceptor
`xcoro::net::acceptor` 用于构建异步 TCP 监听器。通常通过 `acceptor::listen()` 绑定端口并开始监听，再通过 `co_await async_accept()` 等待客户端接入。

```cpp
#include "xcoro/net/acceptor.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
#include "xcoro/net/socket.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

#include <cassert>

int main() {
  xcoro::net::io_context ctx;
  auto listener = xcoro::net::acceptor::listen(ctx, xcoro::net::endpoint::ipv4_any(0));
  const auto listen_ep = listener.native_socket().local_endpoint();

  ctx.run();

  auto server = [&]() -> xcoro::task<> {
    auto peer = co_await listener.async_accept();
    assert(peer.is_open());
    co_return;
  };

  auto client = [&]() -> xcoro::task<> {
    auto sock = xcoro::net::socket::open_tcp(ctx);
    co_await sock.async_connect(listen_ep);
    assert(sock.is_open());
    co_return;
  };

  xcoro::sync_wait(xcoro::when_all(server(), client()));
  ctx.stop();
  return 0;
}
```

### resolver
`xcoro::net::resolver` 提供同步和异步 DNS/地址解析接口。同步版本直接调用 `getaddrinfo()`，异步版本会把阻塞解析工作转移到后台解析线程，并在完成后把结果投递回 `io_context`。

```cpp
#include "xcoro/net/io_context.hpp"
#include "xcoro/net/resolver.hpp"
#include "xcoro/sync_wait.hpp"

#include <cassert>

int main() {
  xcoro::net::io_context ctx;
  ctx.run();

  auto endpoints = xcoro::sync_wait(
      xcoro::net::resolver::async_resolve(ctx, "127.0.0.1", "8080"));

  assert(!endpoints.empty());
  assert(endpoints.front().port() == 8080);

  ctx.stop();
  return 0;
}
```

### cancellation_source
`cancellation_source` 是取消信号的发起端。它持有共享取消状态，并通过 `token()` 生成对应的 `cancellation_token`。调用 `request_cancellation()` 后，所有关联 token 都会进入已取消状态，已经注册的取消回调也会被触发。

```cpp
#include "xcoro/cancellation_source.hpp"

#include <cassert>

int main() {
  xcoro::cancellation_source source;
  auto token = source.token();

  assert(token.can_be_cancelled());
  assert(!token.is_cancellation_requested());

  bool first = source.request_cancellation();
  bool second = source.request_cancellation();

  assert(first);
  assert(!second);
  assert(token.is_cancellation_requested());
  return 0;
}
```

### cancellation_token
`cancellation_token` 是取消信号的观察端。它可以被传入支持取消的 awaitable，例如 `mutex::lock(token)`、`semaphore::acquire(token)`、`condition_variable::wait(mtx, token)`、`io_context` 的 I/O 和 timer 接口等。当取消发生时，这些操作通常会抛出 `xcoro::operation_cancelled`。此外，你也可以直接 `co_await token`，让协程一直等待到取消请求到来。

```cpp
#include "xcoro/cancellation_source.hpp"
#include "xcoro/semaphore.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
#include "xcoro/when_all.hpp"

#include <cassert>

int main() {
  xcoro::semaphore sem(0);
  xcoro::cancellation_source source;
  bool cancelled = false;

  auto waiter = [&]() -> xcoro::task<> {
    try {
      co_await sem.acquire(source.token());
    } catch (const xcoro::operation_cancelled&) {
      cancelled = true;
    }
    co_return;
  };

  auto canceller = [&]() -> xcoro::task<> {
    source.request_cancellation();
    co_return;
  };

  xcoro::sync_wait(xcoro::when_all(waiter(), canceller()));
  assert(cancelled);
  return 0;
}
```
