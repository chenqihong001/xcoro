#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace xcoro::net {

// IPv6本地回环地址 ::1
// 2001:db8::34:7899

class endpoint {
 public:
  endpoint() noexcept : len_(0) {
    std::memset(&storage_, 0, sizeof(storage_));
  }

  static endpoint from_ip_port(std::string_view ip, uint16_t port) {
    endpoint ep;
    if (!ip.empty() && ip.front() == '[' && ip.back() == ']') {
      ip = ip.substr(1, ip.size() - 2);
    }
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
      v4.sin_addr.s_addr = htonl(INADDR_ANY);
      std::memcpy(&ep.storage_, &v4, sizeof(v4));
      ep.len_ = sizeof(v4);
      return ep;
    }
    if (::inet_pton(AF_INET, ip.data(), &v4.sin_addr) == 1) {
      // presentation to network
      std::memcpy(&ep.storage_, &v4, sizeof(v4));
      ep.len_ = sizeof(v4);
      return ep;
    }

    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(port);
    if (ip == "::") {
      v6.sin6_addr = in6addr_any;
      std::memcpy(&ep.storage_, &v6, sizeof(v6));
      ep.len_ = sizeof(v6);
      return ep;
    }
    if (::inet_pton(AF_INET6, ip.data(), &v6.sin6_addr) == 1) {
      std::memcpy(&ep.storage_, &v6, sizeof(v6));
      ep.len_ = sizeof(v6);
      return ep;
    }
    throw std::runtime_error{"invalid ip: " + std::string(ip)};
  }

  static endpoint from_sockaddr(const sockaddr* sa, socklen_t len) {
    if (sa == nullptr || len == 0 || len > sizeof(sockaddr_storage)) {
      throw std::runtime_error{"invalid sockaddr"};
    }
    endpoint ep;
    std::memcpy(&ep.storage_, sa, len);
    ep.len_ = len;
    return ep;
  }

  const sockaddr* data() const noexcept {
    return reinterpret_cast<const sockaddr*>(&storage_);
  }
  sockaddr* data() noexcept {
    return reinterpret_cast<sockaddr*>(&storage_);
  }

  socklen_t size() const noexcept { return len_; }

  int family() const noexcept { return data()->sa_family; }

  std::string ip() const {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (family() == AF_INET) {
      auto* a = reinterpret_cast<const sockaddr_in*>(&storage_);
      ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
      return std::string(buf);
    }
    if (family() == AF_INET6) {
      auto* a = reinterpret_cast<const sockaddr_in6*>(&storage_);
      ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
      return std::string(buf);
    }
    return {};
  }

  uint16_t port() const noexcept {
    if (family() == AF_INET) {
      auto* a = reinterpret_cast<const sockaddr_in*>(&storage_);
      return ntohs(a->sin_port);
    }
    if (family() == AF_INET6) {
      auto* a = reinterpret_cast<const sockaddr_in6*>(&storage_);
      return ntohs(a->sin6_port);
    }
    return 0;
  }
  std::string to_string() const {
    if (family() == AF_INET6) {
      return "[" + ip() + "]:" + std::to_string(port());
    }
    return ip() + ":" + std::to_string(port());
  }

 private:
  sockaddr_storage storage_{};
  socklen_t len_{};
};

}  // namespace xcoro::net