
## Cancellation取消机制
```cpp
class callback_state;
- void invoke() 执行回调

- std::function<void()>callback 回调函数
- std::atomic<bool> invoked{false} 标记是否已经执行过回调了


class cancellation_state;
- bool is_cancellation_requested() 是否已经请求cancel
- bool try_add_callback(const std::shared_ptr<callback_state>&cb) 尝试添加回调函数
- void remove_callback(const std::shared_ptr<callback_state>&cb) 删除指定回调函数
- bool request_cancellation() 请求cancel并执行所有回调

- std::atomic<bool> cancelled{false} 标记是否请求cancel
- std::mutex mutex_ 锁callbacks_数组
- std::vector<std::shared_ptr<callback_state>> callbacks_

- cancellation_state 一个cancellation状态，持有多个取消回调函数


class cancellation_registration;
- cancellation_registration(const cancellation_token& token,Callback&& callback) 在构造函数的时候就要注册指定state的callback
- void deregister() 通过state_指针获得关联的cancellation_state状态，调用state_->remove_callback(callback)删除state中该registration对应的callback，然后reset state_指针，callback_指针
- bool is_registered() 该registration是否注册了callback

- void register_callback(const cancellation_token& token,Callback&& callback) 
- std::shared_ptr<detail::cancellation_state> state_ 关联一个cancellation状态
- std::shared_ptr<detail::callback_state> callback_ 对应一个callback回调函数

- cancellation_registration 关联一个cancellation状态，对应一个callback回调函数



class cancellation_token;
- bool can_be_cancelled() 返回state_是否有效，也就是是否正确关联cancellation_state
- bool is_cancellation_requested() 通过调用关联的state_的同名函数获取是否已经请求cancel

- std::shared_ptr<detail::cancellation_state> state_ 关联一个state不持有

- cancellation_token 关联一个state_


class cancellation_token::awaiter;
- awaiter(cancellation_token token) 
- bool await_ready() {return !token_.can_be_cancelled() || token_.is_cancellation_requested()} 如果该token没有正确关联state或者关联的state已经request cancellation，那么就直接恢复协程，不挂起
- bool await_suspend(std::coroutine_handle<> awaiting) 进入该函数表明该token关联的state没有请求cancellation，挂起协程，保存协程句柄，注册cancellation_registration  回调函数（该回调函数中会保存协程句柄，并负责恢复协程执行）

- cancellation_token token_
- cancellation_registration registration_
- std::coroutine_handle<> awaiting_


class cancellation_source;
- cancellation_source() 创建一个cancellation_state对象
- cancellation_token token() 创建一个关联state_的cancellation_token
- bool request_cancellation() 请求cancellation/request cancellation

- std::shared_ptr<detail::cancellation_state> state_ 持有一个state


class operation_cancelled : public std::exception;
- const char* what() 返回"operation cancelled"

void throw_if_cancellation_requested(const cancellation_token & token) 如果token关联的state请求cancel，则抛出异常

```


## Network网络
```cpp
class io_context;
- void run() 
- void run_in_current_thread()
- void stop()
- task<> schedule()
- void spawn(task<T> t);
- task<> sleep_for(std::chrono::duration<Req,Period> d)
- rask<> sleep_for(std::chrono::duration<Req,Period> d,cancellation_token token)


class endpoint;
- static endpoint from_ip_port(std::string_view ip,uint16_t port)
- static endpoint from_sockaddr(const sockaddr* addr,socklen_t len)
- const sockaddr * data() 
- socklen_t size() 
- int family() 
- std::string ip()
- uint16_t port() 
- std::string to_string()


class byte_buffer;
- size_t size() 
- size_t capacity() 
- std::span<const std::byte> data() 可读区域
- std::span<std::byte> prepare(size_t n) 预留写入区域
- void commit(size_t n) 写入完成
- void consume(size_t n) 读取完成
- void clear() 
- void shrink_to_fit()

struct mutable_buffer;
- std::span<std::byte> bytes

struct const_buffer;
- std::span<const std::byte> bytes


class socket;
- socket(io_context & ctx,int fd)
- static socket open_tcp(io_context & ctx,int family = AF_INET)
- static socket open_udp(io_context & ctx,int family = AF_INET)
- void set_nonblocking(bool on = true) 设置非阻塞
- void set_reuse_addr(bool on = true) 设置地址重用
- void set_reuse_port(bool on = true) 设置端口重用
- void set_tcp_nodelay(bool on = true) 设置tcp Nagle算法
- void bind(const endpoint& ep)
- void listen(int backlog = SOMAXCONN)

- task<> async_connect(const endpoint& ep)
- task<> async_connect(const endpoint& ep,cancellation_token token)

- task<size_t> async_read_some(mutable_buffer buf)
- task<size_t> async_read_some(mutable_buffer buf,cancellation_token token)
- task<size_t> async_read_exact(mutable_buffer buf)
- task<size_t> async_read_exact(mutable_buffer buf,cancellation_token token)

- task<size_t> async_write_some(const_buffer buf);
- task<size_t> async_write_some(const_buffer buf, cancellation_token token);
- task<size_t> async_write_all(const_buffer buf);
- task<size_t> async_write_all(const_buffer buf, cancellation_token token);
- int native_handle() 获取原始文件描述符
- bool is_open()
- void close() 


class acceptor;
- static acceptor bind(io_context & ctx,const endpoint& ep,int backlog=SOMAXCONN)
- socket & native_socket()
- task<socket> async_accept()
- tasl<socket> async_accept(cancellation_token token)

struct resolve_options;
- int family = AF_UNSPEC 未指定地址族



```