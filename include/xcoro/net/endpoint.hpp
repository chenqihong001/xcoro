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

class endpoint {
 public:
  endpoint() noexcept = default;

  static endpoint from_numeric_address(std::string_view address, uint16_t port) {
    endpoint ep;

    if (!address.empty() && address.front() == '[' && address.back() == ']') {
      address = address.substr(1, address.size() - 2);
    }

    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(port);

    if (address.empty() || address == "0.0.0.0") {
      ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
      std::memcpy(&ep.storage_, &ipv4, sizeof(ipv4));
      ep.length_ = sizeof(ipv4);
      return ep;
    }

    if (::inet_pton(AF_INET, address.data(), &ipv4.sin_addr) == 1) {
      std::memcpy(&ep.storage_, &ipv4, sizeof(ipv4));
      ep.length_ = sizeof(ipv4);
      return ep;
    }

    sockaddr_in6 ipv6{};
    ipv6.sin6_family = AF_INET6;
    ipv6.sin6_port = htons(port);

    if (address == "::") {
      ipv6.sin6_addr = in6addr_any;
      std::memcpy(&ep.storage_, &ipv6, sizeof(ipv6));
      ep.length_ = sizeof(ipv6);
      return ep;
    }

    if (::inet_pton(AF_INET6, address.data(), &ipv6.sin6_addr) == 1) {
      std::memcpy(&ep.storage_, &ipv6, sizeof(ipv6));
      ep.length_ = sizeof(ipv6);
      return ep;
    }

    throw std::runtime_error("invalid numeric address: " + std::string(address));
  }

  static endpoint from_ip_port(std::string_view ip, uint16_t port) {
    return from_numeric_address(ip, port);
  }

  static endpoint ipv4_any(uint16_t port) {
    return from_numeric_address("0.0.0.0", port);
  }

  static endpoint ipv6_any(uint16_t port) {
    return from_numeric_address("::", port);
  }

  static endpoint from_sockaddr(const sockaddr* sa, socklen_t len) {
    if (sa == nullptr || len == 0 || len > sizeof(sockaddr_storage)) {
      throw std::runtime_error("invalid sockaddr");
    }

    endpoint ep;
    std::memcpy(&ep.storage_, sa, len);
    ep.length_ = len;
    return ep;
  }

  const sockaddr* data() const noexcept {
    return reinterpret_cast<const sockaddr*>(&storage_);
  }

  sockaddr* data() noexcept { return reinterpret_cast<sockaddr*>(&storage_); }

  socklen_t size() const noexcept { return length_; }

  int family() const noexcept { return data()->sa_family; }

  std::string address_string() const {
    char buffer[INET6_ADDRSTRLEN] = {0};

    if (family() == AF_INET) {
      const auto* address = reinterpret_cast<const sockaddr_in*>(&storage_);
      ::inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer));
      return std::string(buffer);
    }

    if (family() == AF_INET6) {
      const auto* address = reinterpret_cast<const sockaddr_in6*>(&storage_);
      ::inet_ntop(AF_INET6, &address->sin6_addr, buffer, sizeof(buffer));
      return std::string(buffer);
    }

    return {};
  }

  std::string ip() const { return address_string(); }

  uint16_t port() const noexcept {
    if (family() == AF_INET) {
      const auto* address = reinterpret_cast<const sockaddr_in*>(&storage_);
      return ntohs(address->sin_port);
    }

    if (family() == AF_INET6) {
      const auto* address = reinterpret_cast<const sockaddr_in6*>(&storage_);
      return ntohs(address->sin6_port);
    }

    return 0;
  }

  std::string to_string() const {
    if (family() == AF_INET6) {
      return "[" + address_string() + "]:" + std::to_string(port());
    }
    return address_string() + ":" + std::to_string(port());
  }

 private:
  sockaddr_storage storage_{};
  socklen_t length_{0};
};

}  // namespace xcoro::net
