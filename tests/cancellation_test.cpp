#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <thread>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_source.hpp"
#include "xcoro/cancellation_state.hpp"
#include "xcoro/cancellation_token.hpp"
#include "xcoro/sync_wait.hpp"
#include "xcoro/task.hpp"
using namespace xcoro;

TEST(CancellationTokenTest, Default) {
  cancellation_source source;
  auto token = source.token();
  EXPECT_TRUE(token.can_be_cancelled());
  EXPECT_FALSE(token.is_cancellation_requested());
  cancellation_token unini_token;
  EXPECT_FALSE(unini_token.can_be_cancelled());
}

TEST(CancellationTokenTest, RequestCancellation) {
  cancellation_source source;
  auto token = source.token();
  EXPECT_FALSE(token.is_cancellation_requested());
  EXPECT_TRUE(source.request_cancellation());
  EXPECT_TRUE(token.is_cancellation_requested());
  EXPECT_TRUE(source.is_cancellation_requested());
  EXPECT_FALSE(source.request_cancellation());  // 第二次请求取消，应该失败
}

TEST(CancellationTokenTest, RegistrationCallback) {
  cancellation_source source;
  auto token = source.token();
  std::atomic<int> callback_count = 0;
  std::string ret;
  cancellation_registration reg(token, [&callback_count, &ret] {
    callback_count++;
    ret = "success";
  });
  // 触发取消
  source.request_cancellation();
  // 回调应该被调用一次
  EXPECT_EQ(callback_count.load(), 1);
  EXPECT_EQ(ret, std::string("success"));
  source.request_cancellation();
  EXPECT_EQ(callback_count.load(), 1);
}

TEST(CancellationTokenTest, MultipleRegistrations) {
  cancellation_source source;
  auto token = source.token();
  std::atomic_int cnt1 = 0;
  std::atomic_int cnt2 = 0;
  cancellation_registration reg1(token, [&cnt1] {
    cnt1++;
  });
  cancellation_registration reg2(token, [&cnt2] {
    cnt2++;
  });
  source.request_cancellation();
  EXPECT_EQ(cnt1.load(), 1);
  EXPECT_EQ(cnt2.load(), 1);
}

TEST(CancellationTokenTest, RegistrationDestructorUnregisters) {
  cancellation_source source;
  auto token = source.token();
  std::atomic<int> cnt1 = 0;
  {
    cancellation_registration reg(token, [&cnt1] {
      cnt1++;
    });
    // 退出作用域，reg析构自动退出state回调链
  }
  source.request_cancellation();
  EXPECT_EQ(cnt1.load(), 0);
}

// 测试cancellation_registration的移动复制运算符
TEST(CancellationTokenTest, CancellationRegistrationMoveOperation) {
  cancellation_source source;
  auto token = source.token();
  std::atomic<int> cnt1 = 0;
  std::atomic<int> cnt2 = 0;

  cancellation_registration reg1(token, [&cnt1] {
    cnt1++;
  });
  cancellation_registration reg2(token, [&cnt2] {
    cnt2++;
  });
  reg1 = std::move(reg2);  // 这样应该导致原来的reg1失效
  source.request_cancellation();
  EXPECT_EQ(cnt1.load(), 0);
  EXPECT_EQ(cnt2.load(), 1);
}

TEST(CancellationTokenTest, AlreadyCancelledToken) {
  cancellation_source source;
  source.request_cancellation();

  auto token = source.token();

  std::atomic<int> callback_count{0};

  // 回调应该立即执行
  cancellation_registration reg(token, [&callback_count] {
    callback_count.fetch_add(1, std::memory_order_relaxed);
  });

  EXPECT_EQ(callback_count.load(), 1);
}

TEST(CancellationTokenTest, CoAwaitToken) {
  cancellation_source source;
  std::string message;
  std::atomic<bool> waiting{false};
  auto task1 = [&source, &message, &waiting]() -> task<> {
    auto token = source.token();
    waiting.store(true, std::memory_order_release);
    co_await token;  // 等待取消
    message = "stop";
    co_return;
  };

  auto fut = std::async(std::launch::async, [&] {
    sync_wait(task1());
  });

  while (!waiting.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  source.request_cancellation();
  fut.get();
  EXPECT_EQ(message, "stop");
}

TEST(CancellationTokenTest, ThrowIfCancellationRequested) {
  cancellation_source source;
  auto token = source.token();

  // 初始不应该抛出
  EXPECT_NO_THROW(throw_if_cancellation_requested(token));

  // 请求取消后应该抛出
  source.request_cancellation();
  EXPECT_THROW(throw_if_cancellation_requested(token), operation_cancelled);
}

TEST(CancellationTokenTest, ThreadSafetyMultipleRegistrations) {
  cancellation_source source;
  auto token = source.token();

  constexpr int kThreadCount = 10;
  constexpr int kRegistrationsPerThread = 100;
  std::atomic<int> total_callbacks{0};

  std::vector<std::thread> threads;
  std::vector<std::unique_ptr<cancellation_registration>> registrations;
  registrations.reserve(kThreadCount * kRegistrationsPerThread);

  // 创建注册
  for (int i = 0; i < kThreadCount * kRegistrationsPerThread; ++i) {
    registrations.push_back(std::make_unique<cancellation_registration>(
        token, [&total_callbacks] {
          total_callbacks.fetch_add(1, std::memory_order_relaxed);
        }));
  }

  // 启动线程并发触发取消
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&source] {
      source.request_cancellation();
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // 每个回调应该恰好被调用一次
  EXPECT_EQ(total_callbacks.load(), kThreadCount * kRegistrationsPerThread);
}
