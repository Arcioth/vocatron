#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/ring.hpp"
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using vocatron::SpscRing;

TEST_CASE("write then read keeps the bytes in order") {
  SpscRing<std::uint32_t> ring(8);
  CHECK(ring.capacity() == 8);
  std::uint32_t in[5] = {1, 2, 3, 4, 5};
  CHECK(ring.try_write(in, 5) == 5);
  std::uint32_t out[5] = {};
  CHECK(ring.try_read(out, 5) == 5);
  for (int i = 0; i < 5; ++i) {
    CHECK(out[i] == in[i]);
  }
  CHECK(ring.size_approx() == 0);
  CHECK(ring.try_read(out, 1) == 0);
}

TEST_CASE("a full ring accepts nothing more") {
  SpscRing<std::uint8_t> ring(1024);
  std::vector<std::uint8_t> extra(ring.capacity() + 50, 7);
  CHECK(ring.try_write(extra.data(), extra.size()) == ring.capacity());
  CHECK(ring.try_write(extra.data(), 1) == 0);
  std::vector<std::uint8_t> out(10);
  CHECK(ring.try_read(out.data(), out.size()) == out.size());
  CHECK(ring.try_write(extra.data(), 1) == 1);
}

TEST_CASE("values survive wrapping around the buffer") {
  SpscRing<std::uint32_t> ring(8);
  constexpr int kCount = 10000;
  std::thread producer([&] {
    for (std::uint32_t i = 0; i < kCount;) {
      if (ring.try_write(&i, 1) == 1) {
        ++i;
      } else {
        std::this_thread::yield();
      }
    }
  });
  for (std::uint32_t expect = 0; expect < kCount;) {
    std::uint32_t got = 0;
    if (ring.try_read(&got, 1) == 1) {
      CHECK(got == expect);
      ++expect;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
}

TEST_CASE("no drops at 2.4 MS/s of int8 I/Q") {
  // One I/Q sample is two bytes. 2.4 million samples/s is 4.8 MB/s.
  // The producer writes USB-sized chunks and does not retry a short write:
  // that is a drop, the same rule the HackRF callback uses.
  constexpr std::size_t kSampleRate = 2'400'000;
  constexpr std::size_t kSeconds = 2;
  constexpr std::size_t kBytes = kSampleRate * 2 * kSeconds;
  constexpr std::size_t kChunk = 256 * 1024;
  SpscRing<std::uint8_t> ring(1 << 20);

  std::atomic<bool> run{true};
  std::atomic<std::size_t> got{0};
  std::thread consumer([&] {
    std::vector<std::uint8_t> buf(64 * 1024);
    while (true) {
      const std::size_t n = ring.try_read(buf.data(), buf.size());
      if (n == 0) {
        if (!run.load(std::memory_order_acquire)) {
          const std::size_t rest = ring.try_read(buf.data(), buf.size());
          if (rest == 0) {
            break;
          }
          got.fetch_add(rest, std::memory_order_relaxed);
          continue;
        }
        std::this_thread::yield();
        continue;
      }
      got.fetch_add(n, std::memory_order_relaxed);
    }
  });

  std::vector<std::uint8_t> chunk(kChunk, 0x5a);
  std::size_t sent = 0;
  std::size_t dropped = 0;
  const auto start = std::chrono::steady_clock::now();
  const auto slice = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(static_cast<double>(kChunk) / (kSampleRate * 2.0)));
  while (sent < kBytes) {
    const std::size_t n = std::min(kChunk, kBytes - sent);
    const std::size_t wrote = ring.try_write(chunk.data(), n);
    sent += wrote;
    dropped += n - wrote;
    // Pace from now, so a slow write never turns into a burst that overflows the ring.
    std::this_thread::sleep_until(std::chrono::steady_clock::now() + slice);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  run.store(false, std::memory_order_release);
  consumer.join();

  const double seconds = std::chrono::duration<double>(elapsed).count();
  const double samples_per_s = static_cast<double>(got.load()) / 2.0 / seconds;
  CHECK(dropped == 0);
  CHECK(got.load() == kBytes);
  CHECK(samples_per_s >= static_cast<double>(kSampleRate) * 0.9);
}
