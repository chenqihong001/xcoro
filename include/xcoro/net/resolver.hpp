#pragma once
#include <arpa/inet.h>
#include <netdb.h>

#include <future>
#include <vector>

#include "xcoro/cancellation_registration.hpp"
#include "xcoro/cancellation_token.hpp"
#include "xcoro/net/endpoint.hpp"
#include "xcoro/net/io_context.hpp"
#include "xcoro/task.hpp"
namespace xcoro::net {

struct resolve_options {
  int family = AF_UNSPEC;
  int socktype = SOCK_STREAM;
  int flags = AI_ADDRCONFIG;
};

class resolver {
 public:
  static std::vector<endpoint> resolve(std::string_view host, std::string_view service, resolve_options opt = {}) {
    addrinfo hints{};
    hints.ai_family = opt.family;
    hints.ai_socktype = opt.socktype;
    hints.ai_flags = opt.flags;
    std::string host_str(host);
    if (!host_str.empty() && host_str.front() == '[' && host_str.back() == ']') {
      host_str = host_str.substr(1, host_str.size() - 2);
    }
    addrinfo* res = nullptr;
    const char* host_ptr = host_str.empty() ? nullptr : host_str.c_str();
    std::string service_str(service);
    int rc = ::getaddrinfo(host_ptr, service_str.c_str(), &hints, &res);
    if (rc != 0) {
      throw std::runtime_error(::gai_strerror(rc));
    }

    std::vector<endpoint> out;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
      out.push_back(endpoint::from_sockaddr(p->ai_addr, p->ai_addrlen));
    }
    ::freeaddrinfo(res);
    return out;
  }

  static task<std::vector<endpoint>> async_resolve(io_context& ctx, std::string host, std::string service,
                                                   resolve_options opt = {}, cancellation_token token = {}) {
    throw_if_cancellation_requested(token);

    // getaddrinfo 是阻塞调用，先投递到后台线程
    auto fut = std::async(std::launch::async, [host = std::move(host),
                                               service = std::move(service),
                                               opt]() mutable {
      return resolve(host, service, opt);
    });

    // 协作式等待 future，同时让调用方可取消
    while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      throw_if_cancellation_requested(token);
      co_await ctx.sleep_for(std::chrono::milliseconds(1), token);
    }
    co_return fut.get();
  }

 private:
};

}  // namespace xcoro::net