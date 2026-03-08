#pragma once
#include <asm-generic/socket.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "address.hpp"

namespace xcoro::net {
enum class socket_type {
  tcp,
  udp
};

class socket {
 public:
  explicit socket(int fd) : fd_(fd), domain_(0), type_(0) {
    int domain, type, protocol;
    socklen_t len = sizeof(domain);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &len) == 0) {
      domain_ = domain;
    }
    len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
      type_ = type;
    }
  }

  socket(const socket&) = delete;
  socket& operator=(const socket&) = delete;

  socket(socket&& other) noexcept : fd_(other.fd_), domain_(other.domain_), type_(other.type_) {
    other.fd_ = -1;
    other.domain_ = 0;
    other.type_ = 0;
  }

  socket& operator=(socket&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      domain_ = other.domain_;
      type_ = other.type_;
      other.fd_ = -1;
      other.domain_ = 0;
      other.type_ = 0;
    }
    return *this;
  }
  /*
    int socket(int domain, int type, int protocol);
    - protocol:
        - 0: 根据domain和type的组合选择默认协议
        - IPPROTO_TCP: 显式指定TCP协议
        - IPRPOTO_UDP: 显式指定UDP协议
        - IPPROTO_ICMP: 指定ICMP协议（通常配合SOCK_RAW使用）
   */

  explicit socket(int domain, int type, int protocol = 0) : fd_(-1), domain_(domain), type_(type) {
    fd_ = ::socket(domain, type, protocol);
    if (fd_ == -1) {
      throw std::system_error{errno, std::system_category(), "socket creation failed"};
    }
  }

  ~socket() { close(); }

  // 绑定地址
  void bind(const address& addr) {
    if (::bind(fd_, addr.addr(), addr.addr_len()) == -1) {
      throw std::system_error{errno, std::system_category(), "bind failed"};
    }
  }

  // 接收连接
  std::pair<socket, std::shared_ptr<address>> accept() {
    sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    // 值-结果参数：addr_len = sizeof(client_addr) 防止内存溢出
    int client_fd = accept4(fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len,
                            SOCK_NONBLOCK | SOCK_CLOEXEC);
    // 直接返回非阻塞客户端fd
    if (client_fd == -1) {
      throw std::system_error{errno, std::system_category(), "accept failed"};
    }
    auto client_addr_obj = address::from_sockaddr(reinterpret_cast<sockaddr*>(&client_addr), addr_len);
    return {socket{client_fd}, client_addr_obj};
  }

  // 监听
  void listen(int backlog = SOMAXCONN) {
    if (::listen(fd_, backlog) == -1) {
      throw std::system_error{errno, std::system_category(), "listen failed"};
    }
  }

  // 连接
  void connect(const address& addr) { connect_blocking(addr); }

  // 阻塞式连接（遇到EINTR会重试）
  void connect_blocking(const address& addr) {
    for (;;) {
      if (::connect(fd_, addr.addr(), addr.addr_len()) == 0) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error{errno, std::system_category(), "connect failed"};
    }
  }

  // 非阻塞连接：返回true表示立即连接成功，返回false表示正在连接
  bool connect_nonblocking(const address& addr) {
    for (;;) {
      if (::connect(fd_, addr.addr(), addr.addr_len()) == 0) {
        return true;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EINPROGRESS || errno == EALREADY) {
        return false;
      }
      throw std::system_error{errno, std::system_category(), "connect failed"};
    }
  }

  // 关闭读/写
  void shutdown(int how = SHUT_RDWR) {
    if (::shutdown(fd_, how) == -1) {
      throw std::system_error{errno, std::system_category(), "shutdown failed"};
    }
  }

  // 关闭socket
  void close() {
    if (valid()) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  // 设置非阻塞模式
  void set_nonblocking(bool nonblocking = true) {
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags == -1) {
      throw std::system_error(errno, std::system_category(), "fcntl F_GETFL failed");
    }
    if (nonblocking) {
      flags |= O_NONBLOCK;
    } else {
      flags &= ~O_NONBLOCK;
    }
    if (fcntl(fd_, F_SETFL, flags) == -1) {
      throw std::system_error(errno, std::system_category(), "fcnt F_SETFL failed");
    }
  }

  // 设置SO_REUSEADDR
  void set_reuse_addr(bool reuse = true) {
    int opt = reuse ? 1 : 0;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
      throw std::system_error{errno, std::system_category(), "setsockopt SO_REUSEADDR failed"};
    }
  }

  // 设置SO_REUSEPORT
  void set_reuse_port(bool reuse = true) {
    int opt = reuse ? 1 : 0;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
      throw std::system_error{errno, std::system_category(), "setsockopt SO_REUSEPORT failed"};
    }
  }

  // 设置TCP_NODELAY，禁用Nagle算法
  void set_tcp_nodelay(bool nodelay = true) {
    int opt = nodelay ? 1 : 0;
    if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1) {
      throw std::system_error{errno, std::generic_category(), "Failed to set TCP_NODELAY"};
    }
  }  // Nagle算法（合并小数据包，将多个小的数据包合并成一个大包再发送）提高网络带宽利用率

  // 获取本地地址
  std::shared_ptr<address> local_address() const {
    sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len) == -1) {
      // 获取套接字的本地地址信息
      throw std::system_error{errno, std::generic_category(), "Failed to get local address"};
    }
    return address::from_sockaddr(reinterpret_cast<sockaddr*>(&addr), addr_len);
  }

  // 获取远程地址
  std::shared_ptr<address> peer_address() const {
    // sockaddr_storage是一个足够大的结构体，可以容纳任何类型的socket地址
    sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len) == -1) {
      // 获取已连接套接字的对端地址信息
      throw std::system_error{errno, std::generic_category(), "Failed to get peer address"};
    }
    return address::from_sockaddr(reinterpret_cast<sockaddr*>(&addr), addr_len);
  }

  // 获取文件描述符
  int fd() const noexcept { return fd_; }

  // 检查是否有效
  bool valid() const noexcept { return fd_ != -1; }

  static socket create_tcp(ip_version version = ip_version::ipv4);
  static socket create_udp(ip_version version = ip_version::ipv4);

 private:
  int fd_;
  int domain_;
  int type_;

  // Domain 协议域
  // - AF_INET: IPv4
  // - AF_INET6: IPv6
  // - AF_UNIX: Unix域（用于同一台计算机上不同进程之间的高效通信，不经过网络协议栈）

  // Type 套接字类型
  // - SOCK_STREAM: 流式套接字
  // - SOCK_DGRAM: 数据报套接字
  // - SOCK_RAW: 原始套接字（允许直接访问底层网络栈）
};

inline socket socket::create_tcp(ip_version version) {
  int domain = 0;
  switch (version) {
    case ip_version::ipv4:
      domain = AF_INET;
      break;
    case ip_version::ipv6:
      domain = AF_INET6;
      break;
    default:
      throw std::runtime_error("Unsupported ip_version");
  }
  return socket{domain, SOCK_STREAM, IPPROTO_TCP};
}

inline socket socket::create_udp(ip_version version) {
  int domain = 0;
  switch (version) {
    case ip_version::ipv4:
      domain = AF_INET;
      break;
    case ip_version::ipv6:
      domain = AF_INET6;
      break;
    default:
      throw std::runtime_error("Unsupported ip_version");
  }
  return socket{domain, SOCK_DGRAM, IPPROTO_UDP};
}

}  // namespace xcoro::net

/*

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
- sockfd: 套接字文件描述符
- level: 选项定义的层次（SOL_SOCKET、IPPROTO_TCP、IPPROTO_IP）
- optname: 选项名称
- optval: 指向选项的指针
- optlen: 选项值的大小
返回值:
- 成功: 返回0
- 失败: 返回-1，并设置errno

SOL_SOCKET 通用套接字层
- SO_REUSEADDR: 允许重用本地地址（解决TIME_WAIT状态导致的地址占用）
- SO_RCVBUF: 接收缓冲区大小
- SO_SNDBUF: 发送缓冲区大小
- SO_REUSEPORT: 允许端口重用/负载均衡（多进程监听同一端口）
- SO_BROADCAST: 允许发送广播（UDP广播）

IPPROTO_TCP TCP协议层
- TCP_NODELAY: 禁用Nagle算法（减少延迟）
...

IPPROTO_IP IP协议层
- IP_TTL: 设置TTL值（控制数据包跳数）
...


*/
