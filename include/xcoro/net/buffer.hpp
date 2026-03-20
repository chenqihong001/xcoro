#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace xcoro::net {

struct mutable_buffer {
  std::span<std::byte> bytes;
};

struct const_buffer {
  std::span<const std::byte> bytes;
};

class byte_buffer {
 public:
  explicit byte_buffer(size_t initial_capacity = 4096)
      : storage_(std::max<size_t>(initial_capacity, 1)) {}

  bool empty() const noexcept { return size_ == 0; }
  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return storage_.size(); }
  size_t writable_bytes() const noexcept { return capacity() - size_; }
  size_t writable() const noexcept { return writable_bytes(); }

  std::array<std::span<const std::byte>, 2> readable_regions() const noexcept {
    if (size_ == 0) {
      return {std::span<const std::byte>{}, std::span<const std::byte>{}};
    }

    const size_t first_length = std::min(size_, capacity() - read_index_);
    const size_t second_length = size_ - first_length;
    return {std::span<const std::byte>(storage_.data() + read_index_, first_length),
            std::span<const std::byte>(storage_.data(), second_length)};
  }

  std::span<const std::byte> contiguous_readable_region() const noexcept {
    return readable_regions()[0];
  }

  // 兼容旧接口，保留 data() 但明确它只返回第一段连续可读区域。
  std::span<const std::byte> data() const noexcept {
    return contiguous_readable_region();
  }

  std::array<std::span<std::byte>, 2> writable_regions(
      size_t max_bytes = std::numeric_limits<size_t>::max()) {
    if (max_bytes == 0) {
      return {std::span<std::byte>{}, std::span<std::byte>{}};
    }

    if (max_bytes == std::numeric_limits<size_t>::max()) {
      max_bytes = writable_bytes();
    } else {
      ensure_writable(max_bytes);
    }

    const size_t tail = write_index();
    if (tail < read_index_) {
      const size_t length = std::min(max_bytes, read_index_ - tail);
      return {std::span<std::byte>(storage_.data() + tail, length),
              std::span<std::byte>{}};
    }

    const size_t first_length = std::min(max_bytes, capacity() - tail);
    const size_t second_length = std::min(max_bytes - first_length, read_index_);
    return {std::span<std::byte>(storage_.data() + tail, first_length),
            std::span<std::byte>(storage_.data(), second_length)};
  }

  // 兼容旧接口。
  std::array<std::span<std::byte>, 2> prepare_regions(
      size_t n = std::numeric_limits<size_t>::max()) {
    return writable_regions(n);
  }

  std::span<std::byte> prepare_contiguous(size_t n) {
    if (n == 0) {
      return {};
    }

    ensure_writable(n);
    if (writable_contiguous() < n) {
      linearize();
    }

    assert(writable_contiguous() >= n);
    return std::span<std::byte>(storage_.data() + write_index(), n);
  }

  // 兼容旧接口。
  std::span<std::byte> prepare(size_t n) { return prepare_contiguous(n); }

  void commit(size_t n) noexcept {
    assert(n <= writable_bytes());
    size_ += n;
  }

  void consume(size_t n) noexcept {
    assert(n <= size_);
    if (n == 0) {
      return;
    }

    read_index_ = (read_index_ + n) % capacity();
    size_ -= n;
    if (size_ == 0) {
      clear();
    }
  }

  void clear() noexcept {
    read_index_ = 0;
    size_ = 0;
  }

 private:
  size_t write_index() const noexcept { return (read_index_ + size_) % capacity(); }

  size_t writable_contiguous() const noexcept {
    if (size_ == capacity()) {
      return 0;
    }

    const size_t tail = write_index();
    if (tail < read_index_) {
      return read_index_ - tail;
    }
    return capacity() - tail;
  }

  void ensure_writable(size_t n) {
    if (writable_bytes() >= n) {
      return;
    }

    size_t new_capacity = capacity();
    const size_t required = size_ + n;
    while (new_capacity < required) {
      new_capacity *= 2;
    }
    grow_and_linearize(new_capacity);
  }

  void linearize() {
    if (size_ == 0) {
      read_index_ = 0;
      return;
    }

    if (read_index_ + size_ <= capacity()) {
      if (read_index_ != 0) {
        std::memmove(storage_.data(), storage_.data() + read_index_, size_);
        read_index_ = 0;
      }
      return;
    }

    std::vector<std::byte> tmp(size_);
    const auto regions = readable_regions();
    std::memcpy(tmp.data(), regions[0].data(), regions[0].size());
    std::memcpy(tmp.data() + regions[0].size(), regions[1].data(),
                regions[1].size());
    std::memcpy(storage_.data(), tmp.data(), size_);
    read_index_ = 0;
  }

  void grow_and_linearize(size_t new_capacity) {
    std::vector<std::byte> next_storage(new_capacity);
    const auto regions = readable_regions();
    std::memcpy(next_storage.data(), regions[0].data(), regions[0].size());
    std::memcpy(next_storage.data() + regions[0].size(), regions[1].data(),
                regions[1].size());
    storage_.swap(next_storage);
    read_index_ = 0;
  }

  std::vector<std::byte> storage_;
  size_t read_index_{0};
  size_t size_{0};
};

}  // namespace xcoro::net
