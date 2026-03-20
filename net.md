# net 模块实现说明

这份文档对应当前已经落地的 `xcoro::net` 实现，不再只是“重构建议”，而是对真实代码结构的说明文档。  
目标是把下面几件事讲清楚：

- `net` 模块现在分成了哪些层
- 每个类负责什么
- 每个成员变量保存什么状态
- 每个成员函数在调用时做什么
- 各个组件之间是怎么协作的

当前网络模块位于：

- [io_context.hpp](/home/chen/projects/xcoro/include/xcoro/net/io_context.hpp)
- [socket.hpp](/home/chen/projects/xcoro/include/xcoro/net/socket.hpp)
- [acceptor.hpp](/home/chen/projects/xcoro/include/xcoro/net/acceptor.hpp)
- [resolver.hpp](/home/chen/projects/xcoro/include/xcoro/net/resolver.hpp)
- [endpoint.hpp](/home/chen/projects/xcoro/include/xcoro/net/endpoint.hpp)
- [buffer.hpp](/home/chen/projects/xcoro/include/xcoro/net/buffer.hpp)
- [net/detail](/home/chen/projects/xcoro/include/xcoro/net/detail)

## 总体结构

现在的 `net` 模块可以理解成三层：

### 1. 对外接口层

直接给用户使用的类型：

- `io_context`
- `socket`
- `acceptor`
- `resolver`
- `endpoint`
- `mutable_buffer`
- `const_buffer`
- `byte_buffer`

### 2. 协议与对象状态层

这层主要负责“一个 fd 到底由谁管理、读写等待怎么挂、取消怎么生效”：

- `detail::descriptor_state`
- `detail::wait_operation_state`
- `detail::wait_kind`
- `detail::waiter_slot`

### 3. 平台实现层

这层收拢 Linux/epoll/fd 细节：

- `detail::epoll_reactor`
- `detail::timer_queue`
- `detail::fd_ops`
- `detail::write_no_sigpipe`
- `detail::blocking_resolver`
- `detail::io_context_access`

## 一条完整调用链怎么走

以 `socket::async_read_some()` 为例，调用链大致是：

1. `socket::async_read_some()` 先直接调用 `::read`
2. 如果读到了数据，直接返回
3. 如果遇到 `EAGAIN/EWOULDBLOCK`，则 `co_await context().wait_readable(...)`
4. `io_context::wait_readable()` 会构造 `fd_wait_awaiter`
5. `fd_wait_awaiter` 把当前协程注册到 `epoll_reactor`
6. `epoll_reactor` 把该 fd 的等待信息挂到 `descriptor_state`
7. epoll 线程收到事件后，通过 `descriptor_state` 找到对应等待者
8. reactor 把协程句柄放入 `io_context` 的 ready queue
9. `io_context::event_loop()` 在 `drain_ready()` 中恢复协程
10. 协程恢复后重新执行 `::read`

也就是说：

- `socket` 负责高层语义
- `io_context` 负责调度和恢复
- `epoll_reactor` 负责 readiness 事件
- `descriptor_state` 负责 fd 级共享状态

## 对外接口层

## `xcoro::net::io_context`

文件：

- [io_context.hpp](/home/chen/projects/xcoro/include/xcoro/net/io_context.hpp)

### 职责

`io_context` 是整个网络模块的调度中心，它负责：

- 运行事件循环
- 持有 `epoll_reactor`
- 持有 timer queue
- 维护 ready queue
- 提供 `schedule()` 和 `sleep_for()`
- 为 `socket` / `acceptor` / `resolver` 提供“恢复协程”的基础设施
- 保留一组 raw-fd 风格的兼容接口

它不是一个“socket 容器”，也不是一个“线程池”。  
它更接近：

- 调度器
- 事件反应器的拥有者
- 定时器驱动器

### public 成员函数

#### `io_context()`

构造函数会直接初始化 `reactor_`：

- `reactor_(*this)`

这意味着：

- `epoll_fd`
- `wake_fd`

会在 `io_context` 构造完成时一并创建好。

#### `~io_context()`

析构时调用 `stop()`，保证：

- 事件循环停止
- 后台线程回收

它不需要自己显式关闭 epoll fd，因为这部分由 `detail::epoll_reactor` 析构负责。

#### `void run()`

启动后台事件循环线程。

行为：

- 如果 `loop_thread_` 已经存在，则直接返回
- 把 `stopped_` 置为 `false`
- 启动 `std::jthread` 执行 `event_loop()`

适合“把事件循环跑在单独线程里”的场景。

#### `void run_in_current_thread()`

在当前线程直接运行事件循环。

行为：

- 如果 `loop_thread_` 已经存在，则直接返回
- 把 `stopped_` 置为 `false`
- 直接调用 `event_loop()`

适合“当前线程就是 I/O 线程”的场景。

#### `void stop()`

停止事件循环。

行为：

- 通过 `stopped_.exchange(true)` 保证只执行一次
- 调用 `wake()`，把可能阻塞在 `epoll_wait()` 的循环唤醒
- 如果有后台线程，则 `join()`

注意：

- `stop()` 是幂等的
- 它不会关闭已经打开的 socket fd
- socket 资源释放仍由 `socket` 自己负责

#### `task<> schedule()`

把当前协程重新投递到 `io_context` 的 ready queue。

内部通过 `schedule_awaiter` 完成，语义是：

- 把当前协程句柄入队
- 通过 `wake()` 唤醒事件循环
- 稍后由 `drain_ready()` 恢复

它适合做：

- 切回事件循环线程
- 把后续逻辑延后到下一轮调度执行

#### `task<size_t> async_read_some(int fd, void* buffer, size_t count[, token])`

raw-fd 兼容接口，作用是：

- 对任意 POSIX fd 做非阻塞 `read`
- 读不到时等待可读事件

它不是 `socket` 专属接口，所以测试里可以拿来处理：

- `socketpair`
- `pipe`

内部流程：

1. 先直接 `::read`
2. `EINTR` 重试
3. `EAGAIN/EWOULDBLOCK` 时构造一个临时 `descriptor_state`
4. `co_await wait_readable(state, token)`
5. 协程恢复后继续重试 `::read`

#### `task<size_t> async_read_exact(int fd, void* buffer, size_t count[, token])`

对 raw-fd 持续读，直到：

- 读满 `count`
- 或遇到 EOF

它本质上是 `read_some + while(total < count)` 的组合语义。

#### `task<size_t> async_write_all(int fd, const void* buffer, size_t count[, token])`

对 raw-fd 持续写，直到全部写完。

内部流程：

1. 先调用 `detail::write_no_sigpipe`
2. `EINTR` 重试
3. `EAGAIN/EWOULDBLOCK` 时等待可写
4. 继续写剩余部分

#### `task<> async_accept(int fd, cancellation_token token = {})`

这是一个兼容层接口。

它本身不做 `accept()`，只做一件事：

- 等待该 fd 变为可读

现在真正的 `accept()` 语义已经放到 `acceptor::async_accept()` 里。

#### `task<> async_connection(int fd, cancellation_token token = {})`

这也是一个兼容层接口。

它做两件事：

1. 等待 fd 变为可写
2. 检查 `SO_ERROR`

现在真正的连接语义已经主要由 `socket::async_connect()` 来组织。

#### `template <typename T> void spawn(task<T> task_value)`

把一个 `task<T>` 以 detached 方式丢进 `io_context` 里执行。

内部逻辑：

- 先 `co_await schedule()`
- 再 `co_await std::move(inner)`

这里用到 `detail::detached_task` 作为“不关心返回值，只驱动执行”的协程壳。

#### `template <typename Rep, typename Period> task<> sleep_for(duration[, token])`

创建定时等待。

内部流程：

- 计算 `deadline`
- 构造 `timer_awaiter`
- 把 timer state 放入 `timer_queue`
- 到期后恢复协程

带 `token` 的版本支持取消。

### private 嵌套类型

#### `detail::detached_task`

它不是 `io_context` 的成员类，但定义在同一个头文件的 `namespace detail` 下。

用途：

- 给 `spawn()` 提供一个“自动启动、自动结束、不保留结果”的协程载体

成员：

- `promise_type`
- `std::coroutine_handle<promise_type> handle_`

关键点：

- `initial_suspend()` 返回 `std::suspend_never`
- `final_suspend()` 返回 `std::suspend_never`

所以它会立刻开始执行，不需要外部再手动 `resume()`。

#### `schedule_awaiter`

职责：

- 把协程挂到 ready queue

成员：

- `io_context* ctx_`

成员函数：

- `await_ready()`
  总是返回 `false`
- `await_suspend(handle)`
  调用 `ctx_->enqueue_ready(handle)` 并 `ctx_->wake()`
- `await_resume()`
  空实现

#### `fd_wait_awaiter`

职责：

- 代表一次“等待 fd 可读/可写”的挂起操作

成员：

- `io_context* ctx_`
  当前上下文
- `detail::descriptor_state* state_`
  当前等待对应的 descriptor 共享状态
- `detail::wait_kind kind_`
  等待方向，`read` 或 `write`
- `cancellation_token token_`
  取消 token
- `detail::wait_operation_state operation_`
  这次等待自身的状态

成员函数：

- `await_ready()`
  总是返回 `false`
- `await_suspend(handle)`
  如果 token 已取消，则直接走 inline 取消路径  
  否则调用 `reactor_.arm_wait(...)`
- `await_resume()`
  检查 `cancelled` 和 `error`
- `~fd_wait_awaiter()`
  如果协程已经不再等待，但 `operation_` 还未完成，则通知 reactor 取消这次等待

这里最重要的点是：

- `descriptor_state` 记录“这个 fd 当前有哪些等待者”
- `wait_operation_state` 记录“这一次等待自己的完成状态”

二者不是一回事。

#### `timer_awaiter`

职责：

- 表示一次“等到某个 deadline”的挂起操作

成员：

- `io_context* ctx_`
- `std::chrono::steady_clock::time_point deadline_`
- `cancellation_token token_`
- `cancellation_registration registration_`
- `std::shared_ptr<detail::timer_queue::timer_state> state_`

成员函数：

- `await_ready()`
  总是返回 `false`
- `await_suspend(handle)`
  创建 timer state，注册取消，入 timer queue，并唤醒事件循环
- `await_resume()`
  如果 timer 被取消则抛 `operation_cancelled`
- `~timer_awaiter()`
  析构时把 `state_->completed` 置为 `true`，防止重复恢复

### private 成员函数

#### `task<> wait_readable(detail::descriptor_state& state, cancellation_token token = {})`

构造 `fd_wait_awaiter`，等待可读事件。

这是给：

- `socket`
- `acceptor`
- raw-fd 兼容接口

使用的底层 hook。

#### `task<> wait_writable(detail::descriptor_state& state, cancellation_token token = {})`

构造 `fd_wait_awaiter`，等待可写事件。

#### `void unregister_descriptor(detail::descriptor_state& state, int error = EBADF) noexcept`

把某个 fd 从 reactor 中注销，并恢复仍然挂在这个 fd 上的等待者。

它主要被 `socket::close()` 调用。

#### `void enqueue_ready(std::coroutine_handle<> handle) noexcept`

把待恢复协程放入 `ready_` 队列。

#### `void wake() noexcept`

直接转发给 `reactor_.wake()`，唤醒 `epoll_wait()`。

#### `void drain_ready()`

把 `ready_` 队列整体取出，然后逐个 `resume()`。

这里刻意先 `swap` 到局部队列，再恢复协程，避免：

- 持锁恢复协程
- 造成 ready queue 串行化

#### `void resume_due_timers()`

从 `timers_` 收集已到期 timer，并把对应协程入 ready queue。

#### `void event_loop()`

主事件循环。

执行顺序：

1. `reactor_.poll_once(timers_.timeout_ms())`
2. `resume_due_timers()`
3. `drain_ready()`

循环退出后还会再执行一次：

- `resume_due_timers()`
- `drain_ready()`

这样可以尽量把退出前已经 ready 的工作跑掉。

### private 成员变量

#### `detail::timer_queue timers_`

保存所有定时器。

#### `detail::epoll_reactor reactor_`

负责和 epoll/eventfd 交互。

#### `std::mutex ready_mutex_`

保护 `ready_` 队列。

#### `std::queue<std::coroutine_handle<>> ready_`

ready queue，保存即将被恢复的协程句柄。

#### `std::jthread loop_thread_`

后台事件循环线程，仅在 `run()` 模式下使用。

#### `std::atomic_bool stopped_{false}`

标记事件循环是否停止。

## `xcoro::net::socket`

文件：

- [socket.hpp](/home/chen/projects/xcoro/include/xcoro/net/socket.hpp)

### 职责

`socket` 表示一个已经绑定到 `io_context` 的 socket 端点。  
它负责：

- 打开 TCP/UDP socket
- 维护 fd 生命周期
- 提供连接、读、写的高层协程接口
- 提供 socket option 设置接口

### public 构造与生命周期

#### `socket() noexcept = default`

创建一个空 socket，对应：

- `state_ == nullptr`

#### `socket(io_context& ctx, int fd) noexcept`

把一个已有 fd 包装成 `socket`，并创建 `descriptor_state`。

#### `explicit socket(std::shared_ptr<detail::descriptor_state> state) noexcept`

直接接管一个已经构造好的 descriptor state。

这个构造主要给内部流程使用，比如：

- `socket::open_tcp/open_udp`
- `acceptor::async_accept`

#### `socket(socket&&) noexcept = default`

移动构造，转移所有权。

#### `socket& operator=(socket&& other) noexcept`

移动赋值时会先：

- `close()` 当前旧 fd

再接管 `other.state_`。  
这保证旧 fd 不会泄漏。

#### `~socket()`

析构时调用 `close()`。

### public 静态工厂函数

#### `static socket open_tcp(io_context& ctx, int family = AF_INET)`

创建一个：

- `SOCK_STREAM`
- 非阻塞
- `CLOEXEC`

的 TCP socket。

内部调用：

- `detail::create_socket(...)`
- `detail::make_descriptor_state(...)`

#### `static socket open_udp(io_context& ctx, int family = AF_INET)`

与 `open_tcp()` 类似，只是协议改成 UDP。

### public 状态查询

#### `bool is_open() const noexcept`

判断 socket 是否仍然持有有效 fd。

#### `int native_handle() const noexcept`

返回原始 fd；如果未打开则返回 `-1`。

### public 同步控制接口

#### `void bind(const endpoint& ep)`

调用 `::bind` 绑定地址。

#### `void listen(int backlog = SOMAXCONN)`

调用 `::listen`，把 socket 变成监听 socket。

#### `void shutdown(int how = SHUT_RDWR)`

调用 `::shutdown`，关闭读写方向之一或全部方向。

#### `void close() noexcept`

关闭 fd。

内部流程：

1. 调用 `state_->ctx->unregister_descriptor(*state_, EBADF)`
2. `::close(state_->fd)`
3. 把 `state_->fd` 置为 `-1`

这样做的目的不仅是关 fd，还包括：

- 把 reactor 里仍挂着的等待一起清理掉

### public socket option 接口

#### `void set_nonblocking(bool on = true)`

调用 `detail::set_nonblocking`。

#### `void set_reuse_address(bool on = true)`

设置 `SO_REUSEADDR`。

#### `void set_reuse_addr(bool on = true)`

兼容旧命名，内部直接转发到 `set_reuse_address()`。

#### `void set_reuse_port(bool on = true)`

设置 `SO_REUSEPORT`。

#### `void set_tcp_nodelay(bool on = true)`

设置 `TCP_NODELAY`。

### public 异步接口

#### `task<> async_connect(const endpoint& ep, cancellation_token token = {})`

异步连接流程：

1. 先调用 `::connect`
2. 成功则直接返回
3. `EINTR` 重试
4. `EINPROGRESS/EALREADY` 时等待可写
5. 恢复后调用 `check_connect_result()` 检查 `SO_ERROR`

#### `task<size_t> async_read_some(mutable_buffer dst, cancellation_token token = {})`

读一次尽可能多的数据。

语义：

- 可能小于请求长度
- 可能返回 `0`，表示 EOF

#### `task<size_t> async_read_exact(mutable_buffer dst, cancellation_token token = {})`

尽量把目标 buffer 填满，除非遇到 EOF。

#### `task<size_t> async_write_some(const_buffer src, cancellation_token token = {})`

写一次尽可能多的数据。

内部使用 `detail::write_no_sigpipe`，避免写 socket 时触发 `SIGPIPE`。

#### `task<size_t> async_write_all(const_buffer src, cancellation_token token = {})`

循环写直到全部写完。

### public 地址查询接口

#### `endpoint local_endpoint() const`

调用 `getsockname()` 获取本地地址。

#### `endpoint peer_endpoint() const`

调用 `getpeername()` 获取对端地址。

### private 成员函数

#### `io_context& context() const`

从 `state_->ctx` 取出绑定的 `io_context`。  
如果没有上下文则抛异常。

#### `detail::descriptor_state& descriptor() const`

返回 descriptor state 引用。

#### `void ensure_open() const`

如果 socket 未打开则抛异常。

#### `void check_connect_result() const`

通过 `detail::get_socket_error()` 读取 `SO_ERROR`，如果非 0 则抛异常。

### private 成员变量

#### `std::shared_ptr<detail::descriptor_state> state_`

这是 `socket` 最核心的状态。

它保存：

- `ctx`
- `fd`
- epoll 注册信息
- 当前读等待
- 当前写等待

也就是说，`socket` 不再只是“一个裸 fd”，而是“一个带 reactor 共享状态的 fd 包装对象”。

## `xcoro::net::acceptor`

文件：

- [acceptor.hpp](/home/chen/projects/xcoro/include/xcoro/net/acceptor.hpp)

### 职责

`acceptor` 表示监听 socket，对外提供：

- listen socket 的构建
- 异步接收连接

### `listen_options`

成员：

- `int backlog = SOMAXCONN`
  传给 `::listen()` 的 backlog，控制内核监听队列的上限。
- `bool reuse_address = true`
  是否在监听 socket 上打开 `SO_REUSEADDR`。  
  这通常用于降低服务重启时因地址仍处于复用等待状态而绑定失败的概率。
- `bool reuse_port = true`
  是否打开 `SO_REUSEPORT`。  
  当前实现默认开启，方便本地开发和某些多进程共享端口场景。

它用于控制 `acceptor::listen()` 的建 socket 参数。

### public 构造与工厂函数

#### `acceptor(io_context& ctx, socket listen_socket) noexcept`

构造一个 `acceptor`，内部保存：

- `ctx_`
- `socket_`

#### `static acceptor listen(io_context& ctx, const endpoint& ep, listen_options options = {})`

完整执行以下步骤：

1. `socket::open_tcp(ctx, ep.family())`
2. 根据 `options` 设置 `reuse_address`
3. 根据 `options` 设置 `reuse_port`
4. `bind(ep)`
5. `listen(options.backlog)`

所以这个工厂的语义其实是“创建监听器”，而不是单纯 bind。

#### `static acceptor bind(io_context& ctx, const endpoint& ep, int backlog = SOMAXCONN)`

兼容旧接口。

内部直接转发到：

- `listen(ctx, ep, listen_options{backlog, true, true})`

### public 访问接口

#### `socket& socket_ref() noexcept`
#### `const socket& socket_ref() const noexcept`

返回内部监听 socket。

#### `socket& native_socket() noexcept`
#### `const socket& native_socket() const noexcept`

兼容旧命名，同样返回内部监听 socket。

### public 异步接口

#### `task<socket> async_accept(cancellation_token token = {})`

这是 `acceptor` 的核心接口。

流程：

1. 调用 `detail::accept_nonblocking()`
2. 如果成功，直接把新 fd 包装成 `socket`
3. `EINTR` 重试
4. `EAGAIN/EWOULDBLOCK` 时等待监听 socket 可读
5. 恢复后继续 accept

注意这里才是真正的“异步 accept”。  
`io_context::async_accept(int fd)` 只是一个低层兼容等待接口。

### private 成员函数

#### `io_context& ctx()`

从 `ctx_` 取出上下文；如果为空则抛异常。

### private 成员变量

#### `io_context* ctx_`

关联的 `io_context`。

#### `socket socket_`

内部持有的监听 socket。

## `xcoro::net::resolver`

文件：

- [resolver.hpp](/home/chen/projects/xcoro/include/xcoro/net/resolver.hpp)

### 职责

`resolver` 负责域名和服务名解析。  
它提供：

- 同步 `getaddrinfo` 包装
- 通过后台线程实现的异步解析

### `resolve_options`

这是旧风格参数对象，成员有：

- `int family = AF_UNSPEC`
  地址族过滤条件。`AF_UNSPEC` 表示 IPv4/IPv6 都接受。
- `int socktype = SOCK_STREAM`
  socket 类型过滤条件。默认 `SOCK_STREAM`，也就是优先解析 TCP 使用的地址。
- `int flags = AI_ADDRCONFIG`
  原样传给 `getaddrinfo()` 的 flags。默认 `AI_ADDRCONFIG`，让系统倾向返回当前主机可用协议族的地址。

它主要服务于兼容接口：

- `resolve(host, service, options)`
- `async_resolve(ctx, host, service, options, token)`

### `resolve_query`

这是现在更完整的解析参数对象，成员有：

- `std::string host`
  主机部分。可以是域名、纯数字 IP，也可以为空字符串。
- `std::string service`
  服务部分。可以是 `"80"` 这样的端口字符串，也可以是系统服务名。
- `int family = AF_UNSPEC`
  地址族过滤条件，语义与 `resolve_options::family` 一致。
- `int socktype = SOCK_STREAM`
  socket 类型过滤条件，语义与 `resolve_options::socktype` 一致。
- `int flags = AI_ADDRCONFIG`
  传给 `getaddrinfo()` 的附加控制位，语义与 `resolve_options::flags` 一致。

推荐新代码优先使用它。

### public 成员函数

#### `static std::vector<endpoint> resolve(const resolve_query& query)`

同步解析。

流程：

1. 构造 `addrinfo hints`
2. 对 host 做 `normalize_host()`，去掉 IPv6 方括号
3. 调用 `getaddrinfo`
4. 把结果链表转换成 `std::vector<endpoint>`

#### `static std::vector<endpoint> resolve(std::string_view host, std::string_view service, resolve_options options = {})`

兼容接口，内部构造 `resolve_query` 后转发给上面的同步解析函数。

#### `static task<std::vector<endpoint>> async_resolve(io_context& ctx, resolve_query query, cancellation_token token = {})`

异步解析主接口。

它内部 `co_await async_resolve_operation{...}`。

#### `static task<std::vector<endpoint>> async_resolve(io_context& ctx, std::string host, std::string service, resolve_options options = {}, cancellation_token token = {})`

兼容接口，内部组装 `resolve_query` 后再进入主异步流程。

### private 嵌套类 `async_resolve_operation`

这个 awaiter 是整个异步 DNS 的关键。

#### 成员

- `io_context* ctx_`
  解析完成后要恢复回哪个上下文
- `resolve_query query_`
  当前解析请求
- `cancellation_token token_`
  取消 token
- `bool cancelled_inline_`
  标记“在 await_suspend 之前就已经取消”
- `std::shared_ptr<detail::blocking_resolver::resolve_job> job_`
  后台 worker 使用的作业对象

#### 构造函数

保存：

- `ctx`
- `query`
- `token`

#### `~async_resolve_operation()`

如果 job 还没完成，则把它标成：

- `completed = true`
- `cancelled = true`

这是一个兜底清理路径，防止 awaiter 提前销毁时后台状态悬空。

#### `bool await_ready() const noexcept`

当前实现始终返回 `false`，即总走 `await_suspend`。

#### `bool await_suspend(std::coroutine_handle<> handle)`

流程：

1. 如果 token 已取消，则设置 `cancelled_inline_ = true`，返回 `false`
2. 否则构造 `resolve_job`
3. 把 `ctx`、`handle`、`request`、`resolve_fn` 填入 job
4. 如果 token 可取消，则注册取消回调
5. 把 job 提交给 `detail::blocking_resolver::instance()`
6. 返回 `true`，挂起当前协程

#### `std::vector<endpoint> await_resume()`

恢复时检查：

- `cancelled_inline_`
- `job_->cancelled`
- `job_->exception`

如果都没有问题，则返回解析结果。

### private 静态函数 `normalize_host(std::string_view host)`

把形如：

- `[::1]`

的 IPv6 字符串去掉外层方括号，再传给 `getaddrinfo`。

## `xcoro::net::endpoint`

文件：

- [endpoint.hpp](/home/chen/projects/xcoro/include/xcoro/net/endpoint.hpp)

### 职责

`endpoint` 是纯地址值对象，负责：

- 保存 `sockaddr_storage`
- 在 IPv4 / IPv6 之间做统一表示
- 提供地址字符串和端口访问接口

### public 静态构造函数

#### `from_numeric_address(std::string_view address, uint16_t port)`

从数值 IP 构造地址。

支持：

- IPv4
- IPv6
- `"0.0.0.0"`
- `"::"`
- 方括号包裹的 IPv6 地址

不支持主机名解析。  
主机名应先交给 `resolver`。

#### `from_ip_port(std::string_view ip, uint16_t port)`

兼容旧接口，直接转发到 `from_numeric_address()`。

#### `ipv4_any(uint16_t port)`

构造 `0.0.0.0:port`。

#### `ipv6_any(uint16_t port)`

构造 `[::]:port`。

#### `from_sockaddr(const sockaddr* sa, socklen_t len)`

从系统调用拿到的 `sockaddr` 直接构造 `endpoint`。

### public 成员函数

#### `const sockaddr* data() const noexcept`
#### `sockaddr* data() noexcept`

返回底层 `sockaddr` 指针，给：

- `bind`
- `connect`
- `getsockname`
- `getpeername`

等系统调用使用。

#### `socklen_t size() const noexcept`

返回当前地址结构长度。

#### `int family() const noexcept`

返回地址族：

- `AF_INET`
- `AF_INET6`

#### `std::string address_string() const`

把地址转换为纯 IP 字符串，不带端口。

#### `std::string ip() const`

兼容旧命名，等价于 `address_string()`。

#### `uint16_t port() const noexcept`

返回端口号，已经做了 `ntohs()`。

#### `std::string to_string() const`

返回：

- IPv4: `127.0.0.1:8080`
- IPv6: `[::1]:8080`

### private 成员变量

#### `sockaddr_storage storage_`

实际保存地址内容。

#### `socklen_t length_`

保存地址长度。

## `xcoro::net::mutable_buffer` / `const_buffer`

文件：

- [buffer.hpp](/home/chen/projects/xcoro/include/xcoro/net/buffer.hpp)

这两个结构非常轻量，本质只是 `std::span` 包装：

### `mutable_buffer`

成员：

- `std::span<std::byte> bytes`
  一段非拥有型可写字节区间。  
  `socket::async_read_some()` / `async_read_exact()` 会直接把收到的数据写入这块内存。

表示一块可写 buffer。

### `const_buffer`

成员：

- `std::span<const std::byte> bytes`
  一段非拥有型只读字节区间。  
  `socket::async_write_some()` / `async_write_all()` 会把这里面的内容发送出去。

表示一块只读 buffer。

## `xcoro::net::byte_buffer`

文件：

- [buffer.hpp](/home/chen/projects/xcoro/include/xcoro/net/buffer.hpp)

### 职责

`byte_buffer` 是一个环形字节缓冲区，负责：

- 保存已读未消费的数据
- 提供两段式 readable region
- 提供两段式 writable region
- 在需要连续写入时自动线性化

### public 成员函数

#### `explicit byte_buffer(size_t initial_capacity = 4096)`

初始化底层 `storage_`，容量至少为 `1`。

#### `bool empty() const noexcept`

当前是否没有可读数据。

#### `size_t size() const noexcept`

当前可读字节数。

#### `size_t capacity() const noexcept`

底层总容量。

#### `size_t writable_bytes() const noexcept`

剩余可写字节数。

#### `size_t writable() const noexcept`

兼容旧命名，等价于 `writable_bytes()`。

#### `std::array<std::span<const std::byte>, 2> readable_regions() const noexcept`

返回两段式可读区域：

- 第一段从 `read_index_` 开始
- 第二段是环绕回头部后的剩余部分

#### `std::span<const std::byte> contiguous_readable_region() const noexcept`

只返回第一段连续可读区域。

#### `std::span<const std::byte> data() const noexcept`

兼容旧接口。  
当前文档上明确说明：

- 它不是“整个 readable 数据”
- 它只是第一段连续可读区域

#### `std::array<std::span<std::byte>, 2> writable_regions(size_t max_bytes = max)`

返回两段式可写区域。

如果 `max_bytes` 不是默认值，则先保证至少有这么多可写空间。

#### `prepare_regions(size_t n = max)`

兼容旧接口，等价于 `writable_regions(n)`。

#### `std::span<std::byte> prepare_contiguous(size_t n)`

确保可以拿到一段长度为 `n` 的连续可写区域。

如果环形布局无法直接提供连续空间，则：

- 调用 `linearize()`

#### `std::span<std::byte> prepare(size_t n)`

兼容旧接口，等价于 `prepare_contiguous(n)`。

#### `void commit(size_t n) noexcept`

把刚刚写入的 `n` 个字节计入可读区。

#### `void consume(size_t n) noexcept`

消费前面 `n` 个字节。

当数据被消费光时，会调用 `clear()` 重置索引。

#### `void clear() noexcept`

把缓冲区清空：

- `read_index_ = 0`
- `size_ = 0`

### private 成员函数

#### `size_t write_index() const noexcept`

返回当前写入位置：

- `(read_index_ + size_) % capacity()`

#### `size_t writable_contiguous() const noexcept`

返回当前尾部还能连续写多少字节。

#### `void ensure_writable(size_t n)`

如果可写空间不足，则扩容，并通过 `grow_and_linearize()` 重排。

#### `void linearize()`

把当前 ring buffer 的可读数据整理成一段连续内存，从下标 0 开始。

#### `void grow_and_linearize(size_t new_capacity)`

扩容并把原有可读数据线性拷贝到新 buffer。

### private 成员变量

#### `std::vector<std::byte> storage_`

底层存储。

#### `size_t read_index_{0}`

当前可读区起点。

#### `size_t size_{0}`

当前可读字节数。

## 状态与平台实现层

## `xcoro::net::detail::wait_operation_state`

文件：

- [operation_state.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/operation_state.hpp)

### 职责

表示“一次具体等待”的状态。  
例如：

- 某个协程这次正在等某个 fd 可读
- 或正在等某个 fd 可写

### 成员

- `std::coroutine_handle<> handle`
  当前等待对应的协程句柄
- `cancellation_registration registration`
  这次等待注册的取消回调
- `std::atomic_bool completed`
  是否已经完成
- `std::atomic_bool cancelled`
  是否是由于取消完成
- `std::atomic_int error`
  完成时带回的错误码

### 成员函数

#### `void reset(std::coroutine_handle<> new_handle) noexcept`

把一次等待状态重新初始化为“刚开始等待”的状态。

## `xcoro::net::detail::descriptor_state`

文件：

- [descriptor_state.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/descriptor_state.hpp)

### 职责

表示“一个 fd 的共享状态”。

这是当前网络模块最关键的结构之一。  
它把原来的：

- “一个 awaiter 直接塞进 epoll user data”

改成了：

- “一个 fd 对应一个共享状态对象”

这样读写等待就不会相互覆盖。

### `wait_kind`

枚举值：

- `read`
- `write`

表示等待方向。

### `waiter_slot`

成员：

- `wait_operation_state* operation`

表示某个方向当前挂着的等待操作。

当前实现约束是：

- 一个 fd 同时最多一个读等待
- 一个 fd 同时最多一个写等待

### `descriptor_state` 成员

- `io_context* ctx`
  该 fd 归属的 `io_context`
- `int fd`
  原始文件描述符
- `std::mutex mutex`
  保护该 descriptor 状态
- `uint32_t registered_events`
  当前在 epoll 中注册的 event mask
- `bool registered_with_epoll`
  当前是否已注册到 epoll
- `bool closing`
  是否正在关闭/注销
- `waiter_slot read_waiter`
  当前读等待
- `waiter_slot write_waiter`
  当前写等待

### 辅助函数 `make_descriptor_state(io_context& ctx, int fd)`

创建一个 `shared_ptr<descriptor_state>`，供：

- `socket`
- `acceptor`

持有。

## `xcoro::net::detail::epoll_reactor`

文件：

- [epoll_reactor.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/epoll_reactor.hpp)

### 职责

这是 Linux epoll 事件反应器本体，负责：

- 创建和管理 `epoll_fd`
- 创建和管理 `wake_fd`
- 把 fd 的读写等待注册到 epoll
- 在事件到来时恢复对应协程
- 在 fd 关闭时批量清理等待者

### public 构造与析构

#### `explicit epoll_reactor(io_context& ctx)`

初始化：

- `ctx_`
- `epoll_fd_`
- `wake_fd_`

并把 `wake_fd_` 注册进 epoll。

#### `~epoll_reactor()`

关闭：

- `wake_fd_`
- `epoll_fd_`

### public 成员函数

#### `void wake() noexcept`

向 `wake_fd_` 写入一个 `uint64_t`，唤醒阻塞中的 `epoll_wait()`。

#### `bool arm_wait(descriptor_state& state, wait_kind kind, wait_operation_state& operation, std::coroutine_handle<> handle, cancellation_token token)`

注册一次等待。

主要流程：

1. 锁住 `state.mutex`
2. 如果 fd 已关闭或正在关闭，则立即让这次等待带 `EBADF` 返回
3. 检查该方向是否已有等待者
4. 把 `operation` 挂到 `read_waiter` 或 `write_waiter`
5. `update_interest_locked(state)` 更新 epoll 关注事件
6. 如果 token 可取消，则注册取消回调

返回值语义：

- `true`
  说明等待已真正挂起
- `false`
  说明无需挂起，当前协程应继续 inline 执行

#### `void cancel_wait(descriptor_state& state, wait_kind kind, wait_operation_state& operation) noexcept`

取消某次还在等待中的操作。

流程：

1. 锁住 `state`
2. 确认 slot 里挂的是这次 operation
3. 清空 slot
4. 更新 epoll interest
5. 标记 operation 完成且 `cancelled = true`
6. 把协程重新丢回 `io_context`

#### `void unregister_descriptor(descriptor_state& state, int error = EBADF) noexcept`

注销某个 fd，并把其上所有等待一起恢复。

主要在：

- `socket::close()`

时调用。

#### `void poll_once(int timeout_ms)`

执行一次 `epoll_wait()`，然后：

- 处理 wake fd
- 分发普通 fd 事件

### private 成员函数

#### `slot_for(state, kind)`

根据方向返回：

- `state.read_waiter`
- `state.write_waiter`

#### `complete_operation_locked(...)`

统一收尾一次等待操作，设置：

- `registration`
- `cancelled`
- `error`
- `completed`

#### `update_interest_locked(descriptor_state& state)`

根据当前是否存在读/写等待者，更新 epoll event mask。

行为：

- 没有任何等待者时，`EPOLL_CTL_DEL`
- 有等待者时，`ADD` 或 `MOD`

#### `dispatch_descriptor_event(descriptor_state& state, uint32_t events) noexcept`

根据 epoll 事件决定：

- 是否完成读等待
- 是否完成写等待

完成后把对应协程入 ready queue。

#### `drain_wake_fd() noexcept`

把 `wake_fd_` 里的计数读空。

### private 成员变量

#### `static constexpr uint64_t kWakeTag`

用于在 epoll 返回事件时识别：

- 这是 wake fd，不是普通业务 fd

#### `io_context* ctx_`

所属上下文。

#### `int epoll_fd_`

epoll 句柄。

#### `int wake_fd_`

eventfd 唤醒句柄。

## `xcoro::net::detail::timer_queue`

文件：

- [timer_queue.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/timer_queue.hpp)

### 职责

保存所有 timer，并按 deadline 排序。

### `timer_state`

成员：

- `io_context* ctx`
  该 timer 最终要恢复回哪个 `io_context`。  
  这个字段由 `timer_awaiter` 在创建时填入。
- `std::coroutine_handle<> handle`
  到期后需要恢复的协程句柄。
- `std::atomic_bool completed`
  这个 timer 是否已经被“某一方”完成。  
  到期恢复、取消恢复、awaiter 析构防重入，都会依赖它做一次性判定。
- `std::atomic_bool cancelled`
  该 timer 是否是因为取消而结束，而不是正常到期。

它代表一个定时等待的共享状态。

### public 成员函数

#### `void push(deadline, state)`

把定时器插入堆。

#### `int timeout_ms() const noexcept`

返回离最近一个定时器触发还剩多少毫秒：

- 没有定时器时返回 `-1`
- 已到期时返回 `0`

这个值会直接作为 `epoll_wait()` 的 timeout。

#### `void collect_due(std::vector<std::shared_ptr<timer_state>>& out)`

把所有已到期 timer 从堆里取出来，放入 `out`。

### private 成员变量

#### `std::mutex mutex_`

保护 timer heap。

#### `std::priority_queue<timer_item, ..., timer_cmp> heap_`

按最早 deadline 优先的最小堆。

## `xcoro::net::detail::fd_ops`

文件：

- [fd_ops.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/fd_ops.hpp)

### 职责

收拢所有 fd 级小工具，避免把这些细节散落在 `socket` / `acceptor` 里。

### 函数列表

#### `set_nonblocking(int fd, bool on)`

通过 `fcntl(F_GETFL/F_SETFL)` 设置非阻塞标志。

#### `set_cloexec(int fd, bool on)`

通过 `fcntl(F_GETFD/F_SETFD)` 设置 `FD_CLOEXEC`。

#### `set_socket_option(int fd, int level, int name, int value)`

对 `setsockopt` 做一个整型值封装。

#### `create_socket(int family, int type, int protocol)`

创建 socket，并尽量直接带上：

- `SOCK_NONBLOCK`
- `SOCK_CLOEXEC`

如果平台不支持，则 fallback 到：

- 先 `socket()`
- 再 `set_nonblocking()`
- 再 `set_cloexec()`

#### `accept_nonblocking(int listen_fd, sockaddr* addr, socklen_t* addrlen)`

优先使用 `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`。  
如果平台不支持，则 fallback 到：

- `accept()`
- `set_nonblocking()`
- `set_cloexec()`

#### `get_socket_error(int fd)`

读取 `SO_ERROR`。

## `xcoro::net::detail::write_no_sigpipe`

文件：

- [no_sigpipe.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/no_sigpipe.hpp)

### 职责

在写 socket 时避免因为对端关闭而触发 `SIGPIPE`。

### 行为

优先使用：

- `send(..., MSG_NOSIGNAL)`

如果不适用，再 fallback 到：

- 临时屏蔽 `SIGPIPE`
- 调用 `write`
- 必要时清理 pending 的 `SIGPIPE`

这个函数被：

- `socket::async_write_some`
- `socket::async_write_all`
- `io_context::async_write_all`

复用。

## `xcoro::net::detail::io_context_access`

文件：

- [io_context_access.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/io_context_access.hpp)

### 职责

这是一个很小的桥接器，用来让 `detail` 层访问 `io_context` 的私有恢复能力，而不把一堆内部函数直接暴露成 public。

### 函数

#### `enqueue_ready(io_context&, std::coroutine_handle<>)`

调用 `io_context::enqueue_ready()`。

#### `wake(io_context&)`

调用 `io_context::wake()`。

## `xcoro::net::detail::resolve_request`

文件：

- [blocking_resolver.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/blocking_resolver.hpp)

### 职责

这是后台 DNS worker 使用的轻量请求对象。

成员：

- `std::string host`
  要解析的主机名或数字地址。
- `std::string service`
  要解析的服务名或端口字符串。
- `int family`
  地址族过滤条件。
- `int socktype`
  socket 类型过滤条件。
- `int flags`
  透传给 `getaddrinfo()` 的 flags。

## `xcoro::net::detail::blocking_resolver`

文件：

- [blocking_resolver.hpp](/home/chen/projects/xcoro/include/xcoro/net/detail/blocking_resolver.hpp)

### 职责

把阻塞的 `getaddrinfo()` 放到独立后台线程里执行，避免：

- 在协程里 `1ms` 轮询 future
- 把阻塞 DNS 混入 epoll 线程

### `resolve_job`

这是后台 worker 的任务对象。

成员：

- `io_context* ctx`
  完成后要恢复回哪个上下文
- `std::coroutine_handle<> handle`
  等待解析结果的协程
- `resolve_request request`
  请求参数
- `std::function<std::vector<endpoint>(const resolve_request&)> resolve_fn`
  实际执行解析的函数
- `cancellation_registration registration`
  取消回调
- `std::mutex mutex`
  保护 job 内部状态
- `std::optional<std::vector<endpoint>> result`
  成功结果
- `std::exception_ptr exception`
  失败异常
- `bool completed`
  是否已完成
- `bool cancelled`
  是否已取消

### public 成员函数

#### `static blocking_resolver& instance()`

返回全局单例。

当前实现是：

- 一个进程里一个 resolver worker

#### `void submit(std::shared_ptr<resolve_job> job)`

把一个解析任务压入队列，并唤醒后台线程。

### private 成员函数

#### `blocking_resolver()`

构造时启动后台线程，线程入口为 `worker_loop()`。

#### `~blocking_resolver()`

析构时：

- 置 `stopping_ = true`
- 唤醒 worker
- `join()` 线程

#### `void worker_loop()`

后台线程主循环。

流程：

1. 等待队列里有 job
2. 弹出一个 job
3. 如果 job 已完成，则跳过
4. 调用 `resolve_fn`
5. 成功则写入 `result`
6. 失败则写入 `exception`
7. 如果这次是首次完成，则把协程恢复回 `ctx`

### private 成员变量

#### `std::mutex mutex_`

保护 job 队列。

#### `std::condition_variable cv_`

用于 worker 线程等待新任务。

#### `std::deque<std::shared_ptr<resolve_job>> jobs_`

待处理解析任务队列。

#### `bool stopping_`

后台线程退出标志。

#### `std::thread worker_`

后台解析线程。

## 现在这套实现的几个关键语义

### 1. 一个 fd 同时最多一个读等待和一个写等待

这是当前实现明确支持的并发模型：

- 允许一个读等待
- 允许一个写等待
- 不允许两个读等待同时挂在一个 fd 上
- 不允许两个写等待同时挂在一个 fd 上

如果同方向重复等待，`epoll_reactor::arm_wait()` 会抛：

- `std::logic_error`

### 2. socket 关闭时会主动清理挂起操作

`socket::close()` 不只是 `::close(fd)`，它还会：

- 调用 `unregister_descriptor`
- 恢复挂在这个 fd 上的等待者

这样可以避免“fd 已经关闭，但协程还永远挂着”。

### 3. timer 不是单独线程驱动，而是复用 `io_context` 事件循环

定时器不会自己开线程。  
它是通过：

- `timer_queue::timeout_ms()`

来影响 `epoll_wait()` 的 timeout，然后由 `event_loop()` 顺带驱动。

### 4. async resolve 不再是 `std::async + 1ms` polling

现在异步 DNS 是：

- 后台线程执行 `getaddrinfo`
- 完成后把协程恢复到 `io_context`

这样结构和职责都更清楚。

## 当前测试覆盖到的关键路径

测试文件：

- [net_test.cpp](/home/chen/projects/xcoro/tests/net_test.cpp)

当前已经覆盖：

- socket 打开后的 `NONBLOCK/CLOEXEC` 行为
- socket move assignment 旧 fd 关闭
- raw-fd `async_read_exact`
- raw-fd `async_write_all`
- raw-fd 读取消
- pipe 上的写入路径
- `write_no_sigpipe` 相关错误路径
- `byte_buffer` 的 ring 行为
- `acceptor::async_accept`
- `resolver::async_resolve`
- `resolver` 预取消

## 维护时最重要的理解

如果只记住一件事，我建议记住这句：

- `socket` 负责“高层网络语义”，`descriptor_state + epoll_reactor` 负责“fd 事件状态”，`io_context` 负责“恢复协程”

这三个角色不要混掉。  
只要这个边界不被打乱，后面继续扩展：

- UDP 专用接口
- Unix domain socket
- vectored I/O
- 更强的 resolver

都会顺很多。  
