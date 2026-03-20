#pragma once

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace xcoro::net::detail {

inline void set_nonblocking(int fd, bool on) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    throw std::system_error(errno, std::system_category(),
                            "fcntl(F_GETFL) failed");
  }

  const int next = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(fd, F_SETFL, next) == -1) {
    throw std::system_error(errno, std::system_category(),
                            "fcntl(F_SETFL) failed");
  }
}

inline void set_cloexec(int fd, bool on) {
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags == -1) {
    throw std::system_error(errno, std::system_category(),
                            "fcntl(F_GETFD) failed");
  }

  const int next = on ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
  if (::fcntl(fd, F_SETFD, next) == -1) {
    throw std::system_error(errno, std::system_category(),
                            "fcntl(F_SETFD) failed");
  }
}

inline void set_socket_option(int fd, int level, int name, int value) {
  if (::setsockopt(fd, level, name, &value, sizeof(value)) == -1) {
    throw std::system_error(errno, std::system_category(),
                            "setsockopt failed");
  }
}

inline int create_socket(int family, int type, int protocol) {
  int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
  if (fd == -1 && (errno == EINVAL || errno == EPROTONOSUPPORT)) {
    fd = ::socket(family, type, protocol);
    if (fd != -1) {
      set_nonblocking(fd, true);
      set_cloexec(fd, true);
    }
  }

  if (fd == -1) {
    throw std::system_error(errno, std::system_category(), "socket failed");
  }

  return fd;
}

inline int accept_nonblocking(int listen_fd, sockaddr* addr, socklen_t* addrlen) {
  int fd = ::accept4(listen_fd, addr, addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd == -1 && (errno == ENOSYS || errno == EINVAL)) {
    fd = ::accept(listen_fd, addr, addrlen);
    if (fd != -1) {
      set_nonblocking(fd, true);
      set_cloexec(fd, true);
    }
  }
  return fd;
}

inline int get_socket_error(int fd) {
  int error = 0;
  socklen_t len = sizeof(error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1) {
    throw std::system_error(errno, std::system_category(),
                            "getsockopt(SO_ERROR) failed");
  }
  return error;
}

}  // namespace xcoro::net::detail
