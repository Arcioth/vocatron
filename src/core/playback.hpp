#pragma once

// Plays 48 kHz mono float audio. The callback only reads a queue.
// It never allocates and never waits.
//
// The radio hands over audio in lumps of about 55 ms, and its clock is not
// exactly the sound card's clock. So playback keeps a cushion: it waits
// until `prime_frames` are queued before it plays, and if the queue ever
// runs dry it goes quiet and waits for the full cushion again. Each time it
// runs dry counts as one underrun (one audible gap).

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
  // Frames to queue before playing. 0 plays whatever is there.
  std::size_t prime_frames = 0;
  // Only the audio callback touches this.
  bool priming = true;
};

// What the audio callback does, as a plain function so tests can call it.
// Fills `dst` with `frames` samples: queued audio, or silence while priming.
void playback_fill(PlaybackUser &user, float *dst, std::size_t frames);

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
