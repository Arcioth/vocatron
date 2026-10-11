#pragma once

#include <cstddef>
#include <cstdint>

namespace vocatron {

// Mute the speaker when the channel is empty.
//
// An empty FM channel is loud hiss, and AM turns its own noise up, so the
// gate does not look at the audio. It looks at how strong the filtered
// radio wave is. Above `open_dbfs` the audio fades in. After the wave
// drops, the audio stays for a short hang so a gap between words is not cut.
class Squelch {
public:
  Squelch(bool enabled, float open_dbfs, std::uint32_t sample_rate_hz);

  // Scale `audio` in place. Returns true while the gate is open, including
  // the hang after the wave drops.
  bool apply(float *audio, std::size_t count, float channel_dbfs);

  // The screen can move this while audio is already playing.
  // Turning the gate on starts from silence, so a hiss does not slip through.
  void set_level(bool enabled, float open_dbfs);

  bool enabled() const { return enabled_; }
  bool open() const { return open_; }

private:
  bool enabled_ = false;
  bool open_ = false;
  float open_dbfs_ = 0.0f;
  float close_dbfs_ = 0.0f;
  float gain_ = 0.0f;
  float step_ = 1.0f;
  unsigned hang_ = 0;
  unsigned hang_full_ = 0;
};

} // namespace vocatron
