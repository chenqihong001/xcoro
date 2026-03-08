#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
namespace xcoro::net {
enum class ip_version {
  ipv4,
  ipv6,
  unknown
};

// 网络地址类
class address {
 public:
  virtual ~address() = default;
  virtual const sockaddr* addr() const = 0;
  virtual socklen_t addr_len() const = 0;
  virtual std::string to_string() const = 0;
  virtual ip_version version() const = 0;

  // 从字符串解析地址
  static std::shared_ptr<address> from_string(const std::string& ip_str, uint16_t port);
  // 从socket地址结构创建
  static std::shared_ptr<address> from_sockaddr(const sockaddr* addr, socklen_t addr_len);
};

// IPv4地址类
class ipv4_address : public address {
 public:
  ipv4_address(const std::string& ip, uint16_t port) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
      addr_.sin_addr.s_addr = htonl(INADDR_ANY);  // htonl将32位主机字节序转换为网络字节序
    } else {
      if (inet_pton(AF_INET, ip.data(), &addr_.sin_addr) <= 0) {
        throw std::runtime_error{"Invalid IPv4 address: " + ip};
      }
    }
  }
  ipv4_address(const sockaddr_in& addr) : addr_(addr) {}

  const sockaddr* addr() const override {
    return reinterpret_cast<const sockaddr*>(&addr_);
  }

  socklen_t addr_len() const override {
    return sizeof(addr_);
  }

  std::string to_string() const override {
    return "[" + ip() + "]:" + std::to_string(port());
  }

  ip_version version() const override {
    return ip_version::ipv4;
  }

  std::string ip() const {
    char ip_str[INET_ADDRSTRLEN];
    // Internet Number To Presentation: 将二进制IP地址转换为文本字符串
    inet_ntop(AF_INET, &addr_.sin_addr, ip_str, sizeof(ip_str));
    return std::string(ip_str);
  }

  uint16_t port() const {
    return ntohs(addr_.sin_port);  // 网络字节序(2B)转换为主机字节序
  }

 private:
  sockaddr_in addr_;  // IPv4专用地址
};

// IPv6地址类
class ipv6_address : public address {
 public:
  ipv6_address(const std::string& ip, uint16_t port) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin6_family = AF_INET6;
    addr_.sin6_port = htons(port);
    if (ip.empty() || ip == "::") {
      addr_.sin6_addr = in6addr_any;  // IPv6的通配地址
    } else {
      if (inet_pton(AF_INET6, ip.data(), &addr_.sin6_addr) <= 0) {
        throw std::runtime_error{"Invalid IPv6 address: " + ip};
      }
    }
  }

  ipv6_address(const sockaddr_in6& addr) : addr_(addr) {}

  const sockaddr* addr() const override {
    return reinterpret_cast<const sockaddr*>(&addr_);
  }

  socklen_t addr_len() const override {
    return sizeof(addr_);
  }

  std::string to_string() const override {
    return ip() + ":" + std::to_string(port());
  }

  ip_version version() const override {
    return ip_version::ipv6;
  }

  std::string ip() const {
    char ip_str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &addr_.sin6_addr, ip_str, sizeof(ip_str));
    return std::string(ip_str);
  }

  uint16_t port() const {
    return ntohs(addr_.sin6_port);
  }

 private:
  sockaddr_in6 addr_;
};

inline std::shared_ptr<address> address::from_string(const std::string& ip_str, uint16_t port) {
  std::string ip = ip_str;
  if (!ip.empty() && ip.front() == '[' && ip.back() == ']') {
    ip = ip.substr(1, ip.size() - 2);
  }
  // 尝试解析成IPv4
  sockaddr_in addr4{};
  addr4.sin_family = AF_INET;
  addr4.sin_port = htons(port);
  if (ip.empty() || ip == "0.0.0.0") {
    addr4.sin_addr.s_addr = htonl(INADDR_ANY);
    return std::make_shared<ipv4_address>(addr4);
  }
  if (inet_pton(AF_INET, ip.c_str(), &addr4.sin_addr) > 0) {
    return std::make_shared<ipv4_address>(addr4);
  }
  // 尝试解析成IPv6
  sockaddr_in6 addr6{};
  addr6.sin6_family = AF_INET6;
  addr6.sin6_port = htons(port);
  if (ip == "::") {
    addr6.sin6_addr = in6addr_any;
    return std::make_shared<ipv6_address>(addr6);
  }
  if (inet_pton(AF_INET6, ip.c_str(), &addr6.sin6_addr) > 0) {
    return std::make_shared<ipv6_address>(addr6);
  }
  throw std::runtime_error{"Invalid IP address: " + ip_str};
}

inline std::shared_ptr<address> address::from_sockaddr(const sockaddr* addr, socklen_t addr_len) {
  if (addr->sa_family == AF_INET && addr_len >= sizeof(sockaddr_in)) {
    return std::make_shared<ipv4_address>(*reinterpret_cast<const sockaddr_in*>(addr));
  } else if (addr->sa_family == AF_INET6 && addr_len >= sizeof(sockaddr_in6)) {
    return std::make_shared<ipv6_address>(*reinterpret_cast<const sockaddr_in6*>(addr));
  }
  throw std::runtime_error{"Unsupported address family"};
}

}  // namespace xcoro::net
