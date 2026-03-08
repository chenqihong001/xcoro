#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace xcoro::net {
class buffer {
 public:
  explicit buffer(size_t initial_capacity = 1024) : buffer_(initial_capacity), read_index_(0), write_index_(0) {}
  // 获取可读数据大小
  size_t readable_bytes() const { return write_index_ - read_index_; }

  // 获取可写空间大小
  size_t writable_bytes() const { return buffer_.size() - write_index_; }

  // 获取预留空间大小
  size_t prependable_bytes() const { return read_index_; }

  // 获取数据指针
  const char* read_ptr() const { return buffer_.data() + read_index_; }
  char* write_ptr() { return buffer_.data() + write_index_; }

  // span/view
  std::span<const char> readable_span() const {
    return std::span<const char>(read_ptr(), readable_bytes());
  }
  std::span<char> writable_span() {
    return std::span<char>(write_ptr(), writable_bytes());
  }
  std::string_view readable_view() const {
    return std::string_view(read_ptr(), readable_bytes());
  }

  // 已读取指定字节数
  void has_read(size_t len) {
    assert(len <= readable_bytes());
    read_index_ += len;
    if (read_index_ == write_index_) {
      reset();
    }
  }

  // 已写入指定字节数
  void has_written(size_t len) {
    assert(len <= writable_bytes());
    write_index_ += len;
  }

  // 预写入空间
  void ensure_writable(size_t len) {
    if (writable_bytes() < len) {
      make_space(len);
    }
    assert(writable_bytes() >= len);
  }

  // 追加数据
  void append(const void* data, size_t len) {
    ensure_writable(len);
    std::memcpy(write_ptr(), data, len);
    has_written(len);
  }
  void append(const std::string& str) {
    append(str.data(), str.size());
  }
  void append(std::string_view sv) {
    append(sv.data(), sv.size());
  }

  void append(const buffer& other) {
    append(other.read_ptr(), other.readable_bytes());
  }

  // 预置数据（在可读数据前面添加）
  void prepend(const void* data, size_t len) {
    assert(len <= prependable_bytes());
    read_index_ -= len;
    std::memcpy(buffer_.data() + read_index_, data, len);
  }

  // 读取数据（不移动读指针）
  void peek(void* buffer, size_t len) const {
    assert(len <= readable_bytes());
    std::memcpy(buffer, read_ptr(), len);
  }

  // 读取并移除数据
  void retrieve(void* buffer, size_t len) {
    peek(buffer, len);
    has_read(len);
  }

  // 读取为字符串
  std::string retrieve_as_string(size_t len) {
    assert(len <= readable_bytes());
    std::string result(read_ptr(), len);
    has_read(len);
    return result;
  }

  std::string retrieve_all_as_string() {
    return retrieve_as_string(readable_bytes());
  }

  // 重置缓冲区
  void reset() {
    read_index_ = 0;
    write_index_ = 0;
  }

  // 交换缓冲区
  void swap(buffer& other) {
    buffer_.swap(other.buffer_);
    std::swap(read_index_, other.read_index_);
    std::swap(write_index_, other.write_index_);
  }

  // 容量相关
  size_t capacity() const { return buffer_.size(); }

  void shrink() {
    if (readable_bytes() == 0) {
      reset();
      buffer_.shrink_to_fit();
    } else {
      // 移动数据到开头
      std::vector<char> new_buffer(readable_bytes());
      std::memcpy(new_buffer.data(), read_ptr(), readable_bytes());
      buffer_.swap(new_buffer);
      write_index_ = readable_bytes();
      read_index_ = 0;
    }
  }

 private:
  // 确保有足够的可写空间
  void make_space(size_t len) {
    if (writable_bytes() + prependable_bytes() < len) {
      // 需要扩容
      buffer_.resize(write_index_ + len);
    } else {
      // 移动数据到开头
      size_t readable = readable_bytes();
      std::memmove(buffer_.data(), read_ptr(), readable);
      read_index_ = 0;
      write_index_ = read_index_ + readable;
      assert(readable == readable_bytes());
    }
  }

  std::vector<char> buffer_;
  size_t read_index_;
  size_t write_index_;
};

// 环形缓冲区（适用于固定大小的数据流）
class ring_buffer {
 public:
  explicit ring_buffer(size_t capacity)
      : buffer_(capacity), head_(0), tail_(0), size_(0), capacity_(capacity) {
    if (capacity == 0) {
      throw std::invalid_argument("ring_buffer capacity cannot be zero");
    }
  }

  // 获取可读数据大小
  size_t readable_bytes() const { return size_; }

  // 获取可写空间大小
  size_t writable_bytes() const { return capacity_ - size_; }

  // 写入数据
  size_t write(const void* data, size_t len) {
    if (len == 0) return 0;

    len = std::min(len, writable_bytes());
    if (len == 0) return 0;

    // 分两段写入
    size_t first_chunk = std::min(len, capacity_ - tail_);
    std::memcpy(buffer_.data() + tail_, data, first_chunk);

    if (first_chunk < len) {
      size_t second_chunk = len - first_chunk;
      std::memcpy(buffer_.data(), static_cast<const char*>(data) + first_chunk, second_chunk);
    }

    tail_ = (tail_ + len) % capacity_;
    size_ += len;
    return len;
  }

  // 读取数据
  size_t read(void* buffer, size_t len) {
    if (len == 0) return 0;

    len = std::min(len, size_);
    if (len == 0) return 0;

    // 分两段读取
    size_t first_chunk = std::min(len, capacity_ - head_);
    std::memcpy(buffer, buffer_.data() + head_, first_chunk);

    if (first_chunk < len) {
      size_t second_chunk = len - first_chunk;
      std::memcpy(static_cast<char*>(buffer) + first_chunk, buffer_.data(), second_chunk);
    }

    head_ = (head_ + len) % capacity_;
    size_ -= len;
    return len;
  }

  // 查看数据（不移动读指针）
  size_t peek(void* buffer, size_t len) const {
    if (len == 0) return 0;

    len = std::min(len, size_);
    if (len == 0) return 0;

    size_t first_chunk = std::min(len, capacity_ - head_);
    std::memcpy(buffer, buffer_.data() + head_, first_chunk);

    if (first_chunk < len) {
      size_t second_chunk = len - first_chunk;
      std::memcpy(static_cast<char*>(buffer) + first_chunk, buffer_.data(), second_chunk);
    }

    return len;
  }

  // 清空缓冲区
  void clear() {
    head_ = 0;
    tail_ = 0;
    size_ = 0;
  }

  // 是否为空
  bool empty() const { return size_ == 0; }

  // 是否为满
  bool full() const { return size_ == capacity_; }

 private:
  std::vector<char> buffer_;
  size_t head_;
  size_t tail_;
  size_t size_;
  size_t capacity_;
};

}  // namespace xcoro::net
