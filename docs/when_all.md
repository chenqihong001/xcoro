# when_all / when_any

本文档给出 `when_all` 与 `when_any` 的实现思路与核心代码结构，便于理解与维护。  
实现位置：
- `include/xcoroutine/when_all.hpp`
- `include/xcoroutine/when_any.hpp`

---

## when_all 思路

目标：等待所有 awaitable 完成，收集所有结果（支持 `T` / `T&` / `void`）。

### 关键结构

- `when_all_latch`：计数器，用于判断是否全部完成。
- `when_all_task_promise<R>`：对每个 awaitable 的结果做统一处理（值/引用/void/异常）。
- `when_all_task`：封装单个 awaitable 的协程任务。
- `when_all_ready_awaitable`：外层 awaitable，用于等待所有任务完成并返回结果 tuple。

### 核心流程

```cpp
class when_all_latch; // 计数器

class when_all_task_promise<R>
// - start(when_all_latch& latch) 保存 latch 并 resume
// - final_suspend() 时调用 latch->notify_awaitable_completed()
// - result()/rethrow_if_exception() 统一处理值/引用/void 与异常

when_all_task make_when_all_task(Awaitable&& awaitable) {
  if constexpr (awaiter_result_t<Awaitable> == void) {
    co_await std::forward<Awaitable>(awaitable);
    co_return;
  } else {
    co_return co_await std::forward<Awaitable>(awaitable);
  }
}

auto when_all(Awaitable&& ... awaitables) {
  auto tasks = std::make_tuple(make_when_all_task(std::forward<Awaitable>(awaitables))...);
  return when_all_ready_awaitable(tasks);
}

co_await when_all_ready_awaitable;
-> try_await(awaiting_coroutine)
   - 保存 awaiting_coroutine
   - 依次 start() 所有任务
   - latch 计数为 0 时恢复 awaiting_coroutine
```

---

## when_any 思路

目标：等待 **第一个完成的 awaitable**，返回它的结果（或异常）。

### 设计要点

1. **first_completion_tracker**：原子地记录“第一个完成者”的索引 + 结果/异常。
2. **event 通知**：第一个完成的任务触发 `event.set()` 唤醒等待者。
3. **包装任务**：为每个 awaitable 创建一个 wrapper 协程，完成后竞争“第一名”。
4. **取消语义**：
   - 默认不取消其他任务，未完成的任务会继续运行；
   - 提供重载版本，可在 `when_any` 完成后 **请求取消** 其它任务；
   - `when_all` 提供 **失败时取消** 重载（有任务抛异常即请求取消）。
   - 当前实现使用 `detached_task` 包装 awaitable，保证生命周期安全。

### 返回类型

- **参数包版本**：`when_any(Awaitables&&...)`  
  返回 `when_any_result<Awaitables...>`：
  ```cpp
  struct when_any_result<...> {
    size_t index;
    std::variant<storage_of<R0>, storage_of<R1>, ...> result;
    template<size_t I> decltype(auto) get(); // 取第 I 个结果
  };
  ```

- **range 版本**：`when_any(RangeType)`  
  返回 `when_any_range_result<T>`：
  ```cpp
  struct when_any_range_result<T> {
    size_t index;
    storage_of<R> result;
    decltype(auto) get();
  };
  ```

### 核心流程（伪代码）

```cpp
task<when_any_result<...>> when_any(Awaitables&&... awaitables) {
  first_completion_tracker<result_t> tracker;
  event notify;

  // 为每个 awaitable 创建 wrapper task 并立即 resume
  auto tasks = std::make_tuple(
      when_any_task<0>(awaitables...), when_any_task<1>(awaitables...) ...);
  std::apply([](auto&... t){ (t.handle().resume(), ...); }, tasks);

  // 等待第一个完成者
  co_await notify;
  if (tracker.has_exception()) rethrow;
  co_return tracker.take_result();
}

task<void> when_any_task<I>(awaitable) {
  try {
    auto r = co_await awaitable;
    if (tracker.try_become_first(I)) {
      tracker.set_result(make_result(I, r));
      notify.set();
    }
  } catch (...) {
    if (tracker.try_become_first(I)) {
      tracker.set_exception(std::current_exception());
      notify.set();
    }
  }
}
```

### 实现位置

- `include/xcoroutine/when_any.hpp`

---

## 使用示例

```cpp
auto r = co_await xcoro::when_any(task1(), task2(), task3());
if (r.index == 1) {
  auto v = r.get<1>(); // 返回 task2 的结果
}

std::vector<xcoro::task<int>> tasks = {task1(), task2()};
auto r2 = co_await xcoro::when_any(tasks);
std::cout << "first index = " << r2.index << ", value = " << r2.get() << "\n";
```

### 取消联动示例

```cpp
xcoro::cancellation_source src;
auto token = src.token();

auto r = co_await xcoro::when_any(src, op1(token), op2(token));

try {
  co_await xcoro::when_all(src, op1(token), op2(token));
} catch (const std::exception&) {
  // 其中一个任务抛异常会触发取消请求
}
```
