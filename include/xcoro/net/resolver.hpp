#pragma once

#include <arpa/inet.h>
#include <netdb.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/detail/blocking_resolver.hpp"
#include "xcoro/net/detail/io_context_access.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
#include "xcoro/task.hpp"

namespace xcoro::net {

struct resolve_options {
  int family = AF_UNSPEC;
  int socktype = SOCK_STREAM;
  int flags = AI_ADDRCONFIG;
};

struct resolve_query {
  std::string host;
  std::string service;
  int family = AF_UNSPEC;
  int socktype = SOCK_STREAM;
  int flags = AI_ADDRCONFIG;
};

class resolver {
 public:
  static std::vector<endpoint> resolve(const resolve_query& query) {
    addrinfo hints{};
    hints.ai_family = query.family;
    hints.ai_socktype = query.socktype;
    hints.ai_flags = query.flags;

    std::string host = normalize_host(query.host);
    std::string service = query.service;
    addrinfo* result = nullptr;
    const char* host_ptr = host.empty() ? nullptr : host.c_str();
    const int rc = ::getaddrinfo(host_ptr, service.c_str(), &hints, &result);
    if (rc != 0) {
      throw std::runtime_error(::gai_strerror(rc));
    }

    std::vector<endpoint> endpoints;
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
      endpoints.push_back(
          endpoint::from_sockaddr(current->ai_addr, current->ai_addrlen));
    }
    ::freeaddrinfo(result);
    return endpoints;
  }

  static std::vector<endpoint> resolve(std::string_view host,
                                       std::string_view service,
                                       resolve_options options = {}) {
    return resolve(resolve_query{std::string(host), std::string(service),
                                 options.family, options.socktype,
                                 options.flags});
  }

  static task<std::vector<endpoint>> async_resolve(io_context& ctx,
                                                   resolve_query query,
                                                   cancellation_token token = {}) {
    co_return co_await async_resolve_operation{ctx, std::move(query),
                                               std::move(token)};
  }

  static task<std::vector<endpoint>> async_resolve(io_context& ctx,
                                                   std::string host,
                                                   std::string service,
                                                   resolve_options options = {},
                                                   cancellation_token token = {}) {
    co_return co_await async_resolve_operation{
        ctx,
        resolve_query{std::move(host), std::move(service), options.family,
                      options.socktype, options.flags},
        std::move(token)};
  }

 private:
  class async_resolve_operation {
   public:
    async_resolve_operation(io_context& ctx, resolve_query query,
                            cancellation_token token)
        : ctx_(&ctx), query_(std::move(query)), token_(std::move(token)) {}

    ~async_resolve_operation() {
      if (job_ == nullptr) {
        return;
      }

      std::lock_guard lock(job_->mutex);
      if (!job_->completed) {
        job_->completed = true;
        job_->cancelled = true;
      }
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
      if (token_.can_be_cancelled() && token_.is_cancellation_requested()) {
        cancelled_inline_ = true;
        return false;
      }

      job_ = std::make_shared<detail::blocking_resolver::resolve_job>();
      job_->ctx = ctx_;
      job_->handle = handle;
      job_->request.host = query_.host;
      job_->request.service = query_.service;
      job_->request.family = query_.family;
      job_->request.socktype = query_.socktype;
      job_->request.flags = query_.flags;
      job_->resolve_fn = [](const detail::resolve_request& request) {
        return resolver::resolve(resolve_query{request.host, request.service,
                                              request.family, request.socktype,
                                              request.flags});
      };

      if (token_.can_be_cancelled()) {
        job_->registration = cancellation_registration(
            token_, [job = job_]() noexcept {
              std::coroutine_handle<> ready;
              io_context* ready_ctx = nullptr;
              {
                std::lock_guard lock(job->mutex);
                if (job->completed) {
                  return;
                }
                job->completed = true;
                job->cancelled = true;
                ready = job->handle;
                ready_ctx = job->ctx;
              }

              if (ready_ctx != nullptr && ready) {
                detail::io_context_access::enqueue_ready(*ready_ctx, ready);
                detail::io_context_access::wake(*ready_ctx);
              }
            });
      }

      detail::blocking_resolver::instance().submit(job_);
      return true;
    }

    std::vector<endpoint> await_resume() {
      if (cancelled_inline_) {
        throw operation_cancelled{};
      }

      if (job_ == nullptr) {
        return {};
      }

      std::lock_guard lock(job_->mutex);
      if (job_->cancelled) {
        throw operation_cancelled{};
      }
      if (job_->exception) {
        std::rethrow_exception(job_->exception);
      }
      return job_->result ? std::move(*job_->result) : std::vector<endpoint>{};
    }

   private:
    io_context* ctx_ = nullptr;
    resolve_query query_;
    cancellation_token token_;
    bool cancelled_inline_ = false;
    std::shared_ptr<detail::blocking_resolver::resolve_job> job_;
  };

  static std::string normalize_host(std::string_view host) {
    std::string normalized(host);
    if (!normalized.empty() && normalized.front() == '[' &&
        normalized.back() == ']') {
      normalized = normalized.substr(1, normalized.size() - 2);
    }
    return normalized;
  }
};

}  // namespace xcoro::net
