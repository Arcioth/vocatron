#pragma once

// A queue between exactly two threads: one pushes, one pops.
//
// The USB thread has to return immediately or the radio drops samples.
// This buffer lets that thread copy bytes in and go back to USB, while
// another thread takes them out and does the slow work. No lock: each
// thread only writes its own counter, so they never wait on each other.
// If the buffer is full, the push gives back how many items fit. The
// caller counts the rest as dropped instead of waiting.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace vocatron {

template <typename T> class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "SpscRing copies items with memcpy");

public:
  // capacity is rounded up to a power of two so the index can wrap with a mask.
  explicit SpscRing(size_t min_capacity) : slots_(round_pow2(min_capacity)) {}

  size_t capacity() const noexcept { return slots_.size(); }

  // How many items are sitting in the buffer. Either thread may call this.
  // The number can change before the caller uses it.
  size_t size_approx() const noexcept {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
  }

  // Copy up to n items in. Returns how many were copied. Only the producer calls this.
  size_t try_write(const T *src, size_t n) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t free = slots_.size() - (head - tail);
    if (n > free) {
      n = free;
    }
    if (n == 0) {
      return 0;
    }
    const size_t index = head & (slots_.size() - 1);
    const size_t first = n < slots_.size() - index ? n : slots_.size() - index;
    std::memcpy(slots_.data() + index, src, first * sizeof(T));
    if (n > first) {
      std::memcpy(slots_.data(), src + first, (n - first) * sizeof(T));
    }
    head_.store(head + n, std::memory_order_release);
    return n;
  }

  // Copy up to n items out. Returns how many were copied. Only the consumer calls this.
  size_t try_read(T *dst, size_t n) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t have = head - tail;
    if (n > have) {
      n = have;
    }
    if (n == 0) {
      return 0;
    }
    const size_t index = tail & (slots_.size() - 1);
    const size_t first = n < slots_.size() - index ? n : slots_.size() - index;
    std::memcpy(dst, slots_.data() + index, first * sizeof(T));
    if (n > first) {
      std::memcpy(dst + first, slots_.data(), (n - first) * sizeof(T));
    }
    tail_.store(tail + n, std::memory_order_release);
    return n;
  }

private:
  static size_t round_pow2(size_t n) {
    size_t p = 2;
    while (p < n && p < (size_t{1} << 31)) {
      p <<= 1;
    }
    return p;
  }

  std::vector<T> slots_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace vocatron
