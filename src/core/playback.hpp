#pragma once

// Plays 48 kHz mono float audio. The callback only reads a queue.
// If the queue is empty it writes silence and counts an underrun.
// It never allocates and never waits.

#include "ring.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace vocatron {

struct PlaybackUser {
  SpscRing<float> *ring = nullptr;
  std::atomic<std::uint64_t> *underruns = nullptr;
  std::atomic<std::uint64_t> *played = nullptr;
};

class Speakers {
public:
  Speakers();
  ~Speakers();

  Speakers(const Speakers &) = delete;
  Speakers &operator=(const Speakers &) = delete;

  // `which` is "pulse" (PipeWire / PulseAudio) or "null" (discard, for tests).
  // On success the device is open and stopped. `backend_name` may be null.
  bool open(PlaybackUser *user, const char *which, const char **backend_name);
  bool start();
  void stop();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vocatron
