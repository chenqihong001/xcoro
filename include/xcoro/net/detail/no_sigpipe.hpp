#pragma once

#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>

namespace xcoro::net::detail {

// 安全的写入函数，防止写入时因对端关闭连接而触发SIGPIPE信号导致进程终止
// 简化版本：假设所有平台都支持MSG_NOSIGNAL
inline ssize_t write_no_sigpipe(int fd, const void* buffer, size_t count) {
  // 使用send()系统调用，带有MSG_NOSIGNAL标志
  // MSG_NOSIGNAL: 如果连接断开，返回EPIPE错误而不发送SIGPIPE信号
  ssize_t n = ::send(fd, buffer, count, MSG_NOSIGNAL);

  // 如果send()成功，直接返回
  if (n >= 0) {
    return n;
  }

  // send()失败，检查错误原因
  // ENOTSOCK: 文件描述符不是套接字（比如是普通文件）
  // EPERM: 操作不允许（某些系统对非套接字使用send()返回此错误）
  if (errno == ENOTSOCK || errno == EPERM) {
    // 回退到普通的write()系统调用
    // 对于普通文件，write()不会产生SIGPIPE
    return ::write(fd, buffer, count);
  }

  return -1;
}

}  // namespace xcoro::net::detail