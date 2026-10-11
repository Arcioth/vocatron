#include "core/playback.hpp"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

TEST_CASE("null playback drains the audio queue") {
  vocatron::SpscRing<float> ring(8192);
  std::vector<float> tone(4000);
  for (std::size_t i = 0; i < tone.size(); ++i) {
    tone[i] = 0.2f * std::sin(2.0f * 3.14159265f * 1000.0f * static_cast<float>(i) / 48000.0f);
  }
  REQUIRE(ring.try_write(tone.data(), tone.size()) == tone.size());

  std::atomic<std::uint64_t> underruns{0};
  std::atomic<std::uint64_t> played{0};
  vocatron::PlaybackUser user;
  user.ring = &ring;
  user.underruns = &underruns;
  user.played = &played;

  vocatron::Speakers speakers;
  const char *backend = "";
  REQUIRE(speakers.open(&user, "null", &backend));
  REQUIRE(speakers.start());

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (played.load(std::memory_order_relaxed) < tone.size() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  speakers.stop();

  CHECK(played.load(std::memory_order_relaxed) >= tone.size());
  CHECK(ring.size_approx() == 0);
}
