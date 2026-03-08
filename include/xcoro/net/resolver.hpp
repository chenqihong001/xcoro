#pragma once
#include <netdb.h>

#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

#include "xcoroutine/net/address.hpp"

namespace xcoro::net {

class resolver {
 public:
  static std::vector<std::shared_ptr<address>> resolve(std::string_view host,
                                                       uint16_t port,
                                                       int family = AF_UNSPEC,
                                                       int socktype = SOCK_STREAM,
                                                       int flags = AI_ADDRCONFIG) {
    std::string host_str(host);
    if (!host_str.empty() && host_str.front() == '[' && host_str.back() == ']') {
      host_str = host_str.substr(1, host_str.size() - 2);
    }

    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_flags = flags;

    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    const char* host_ptr = host_str.empty() ? nullptr : host_str.c_str();
    int rc = ::getaddrinfo(host_ptr, service.c_str(), &hints, &res);
    if (rc != 0) {
      throw std::runtime_error(gai_strerror(rc));
    }

    std::vector<std::shared_ptr<address>> out;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
      out.push_back(address::from_sockaddr(p->ai_addr, p->ai_addrlen));
    }
    ::freeaddrinfo(res);
    return out;
  }
};

}  // namespace xcoro::net
