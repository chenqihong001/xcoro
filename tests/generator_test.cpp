#include "xcoro/generator.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

using namespace xcoro;

TEST(GeneratorTest, Basic) {
  auto gen = []() -> generator<int> {
    for (int i = 0; i < 5; ++i) {
      co_yield i;
    }
  };
  std::vector<int> result;
  for (int val : gen()) {
    result.push_back(val);
  }
  std::vector<int> expect{0, 1, 2, 3, 4};
  EXPECT_EQ(result, expect);
}

TEST(GeneratorTest, EmptyGenerator) {
  auto gen = []() -> generator<int> { co_return; };
  std::vector<int> result;
  for (int val : gen()) {
    result.push_back(val);
  }
  EXPECT_TRUE(result.empty());
}

// 斐波那契
TEST(GeneratorTest, FibonacciSeq) {
  auto fibonacci = [](int limit) -> generator<long long> {
    long long a = 0, b = 1;
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
  std::vector<long long> expected = {0, 1, 1, 2, 3, 5, 8, 13, 21, 34};
  EXPECT_EQ(expected, fib_nums);
}

// 异常抛出测试
TEST(Generator, ExceptionHandling) {
  auto throw_gen = []() -> generator<int> {
    co_yield 1;
    co_yield 2;
    throw std::runtime_error{"Test Generator Exception"};
    co_yield 3;
  };
  auto gen = throw_gen();
  auto it = gen.begin();
  EXPECT_EQ(*it, 1);
  ++it;
  EXPECT_EQ(*it, 2);
  EXPECT_THROW(++it, std::runtime_error);
}

// 引用语义测试
TEST(Generator, ReferenceSemantic) {
  std::vector<int> source = {1, 2, 3, 4};
  auto ref_gen = [&source]() -> generator<int&> {
    for (int& val : source) {
      co_yield val;
    }
  };
  auto gen = [&source]() -> generator<int> {
    for (int& val : source) {
      co_yield val;
    }
  };
  // 无论generator<int>还是generator<int&> co_yield都是返回引用&
  int i = 0;
  for (const int& val : ref_gen()) {
    EXPECT_EQ(std::addressof(val), std::addressof(source[i++]));
  }
  i = 0;
  for (const int& val : gen()) {
    EXPECT_EQ(std::addressof(val), std::addressof(source[i++]));
  }
}

TEST(GeneratorTest, MoveOnlyType) {
  auto move_gen = []() -> generator<std::unique_ptr<int>> {
    co_yield std::make_unique<int>(1);
    co_yield std::make_unique<int>(2);
    co_yield std::make_unique<int>(3);
  };

  std::vector<int> result;
  for (auto& ptr : move_gen()) {
    result.push_back(*ptr);
  }

  EXPECT_EQ(result, std::vector<int>({1, 2, 3}));
}