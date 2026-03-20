#pragma once

#include <netdb.h>

#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/detail/io_context_access.hpp"
#include "xcoro/net/endpoint.hpp"

namespace xcoro::net {

class io_context;

}  // namespace xcoro::net

namespace xcoro::net::detail {

struct resolve_request {
  std::string host;
  std::string service;
  int family = AF_UNSPEC;
  int socktype = SOCK_STREAM;
  int flags = AI_ADDRCONFIG;
};

class blocking_resolver {
 public:
  struct resolve_job {
    io_context* ctx = nullptr;
    std::coroutine_handle<> handle{};
    resolve_request request;
    std::function<std::vector<endpoint>(const resolve_request&)> resolve_fn;
    cancellation_registration registration{};
    std::mutex mutex;
    std::optional<std::vector<endpoint>> result;
    std::exception_ptr exception;
    bool completed = false;
    bool cancelled = false;
  };

  static blocking_resolver& instance() {
    static blocking_resolver resolver;
    return resolver;
  }

  blocking_resolver(const blocking_resolver&) = delete;
  blocking_resolver& operator=(const blocking_resolver&) = delete;

  void submit(std::shared_ptr<resolve_job> job) {
    {
      std::lock_guard lock(mutex_);
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

 private:
  blocking_resolver() : worker_([this] { worker_loop(); }) {}

  ~blocking_resolver() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void worker_loop() {
    while (true) {
      std::shared_ptr<resolve_job> job;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (stopping_ && jobs_.empty()) {
          return;
        }
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }

      if (job == nullptr) {
        continue;
      }

      {
        std::lock_guard lock(job->mutex);
        if (job->completed) {
          continue;
        }
      }

      bool should_resume = false;
      try {
        auto result = job->resolve_fn(job->request);
        std::lock_guard lock(job->mutex);
        if (!job->completed) {
          job->result = std::move(result);
          job->completed = true;
          should_resume = true;
        }
      } catch (...) {
        std::lock_guard lock(job->mutex);
        if (!job->completed) {
          job->exception = std::current_exception();
          job->completed = true;
          should_resume = true;
        }
      }

      if (!should_resume) {
        continue;
      }

      std::coroutine_handle<> handle;
      io_context* ctx = nullptr;
      {
        std::lock_guard lock(job->mutex);
        handle = job->handle;
        ctx = job->ctx;
      }

      if (ctx != nullptr && handle) {
        io_context_access::enqueue_ready(*ctx, handle);
        io_context_access::wake(*ctx);
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<resolve_job>> jobs_;
  bool stopping_ = false;
  std::thread worker_;
};

}  // namespace xcoro::net::detail
