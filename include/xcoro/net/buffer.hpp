#pragma once
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>
namespace xcoro::net {
struct mutable_buffer {
  std::span<std::byte> bytes;
};

struct const_buffer {
  std::span<std::byte> bytes;
};

class byte_buffer {
 public:
  explicit byte_buffer(size_t initial = 4096) : buf_(initial), read_(0), write_(0) {}
  size_t size() const noexcept { return write_ - read_; }
  size_t capacity() const noexcept { return buf_.size(); }
  size_t writable() const noexcept { return buf_.size() - write_; }

  std::span<const std::byte> data() const noexcept {
    return std::span<const std::byte>(buf_.data() + read_, size());
  }

  std::span<std::byte> prepare(size_t n) {
    ensure_writable(n);
    return std::span<std::byte>(buf_.data() + write_, n);
  }
  void commit(std::size_t n) noexcept {
    assert(n <= writable());
    write_ += n;
  }

  void consume(std::size_t n) noexcept {
    assert(n <= size());
    read_ += n;
    if (read_ == write_) {
      clear();
    }
  }
  void clear() noexcept {
    read_ = 0;
    write_ = 0;
  }

 private:
  void ensure_writable(std::size_t n) {
    if (writable() >= n) {
      return;
    }
    if (read_ + writable() >= n) {
      const auto s = size();
      std::memmove(buf_.data(), buf_.data() + read_, s);
      read_ = 0;
      write_ = s;
      return;
    }
    buf_.resize(write_ + n);
  }

  std::vector<std::byte> buf_;
  size_t read_;
  size_t write_;
};

}  // namespace xcoro::net