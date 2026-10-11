#include "squelch.hpp"

#include <algorithm>

namespace vocatron {

namespace {
constexpr float kHysteresisDb = 4.0f;
constexpr float kHangSeconds = 0.30f;
constexpr float kRampSeconds = 0.010f;
} // namespace

Squelch::Squelch(bool enabled, float open_dbfs, std::uint32_t sample_rate_hz)
    : enabled_(enabled), open_(!enabled), open_dbfs_(open_dbfs),
      close_dbfs_(open_dbfs - kHysteresisDb) {
  const float rate = sample_rate_hz == 0 ? 48000.0f : static_cast<float>(sample_rate_hz);
  hang_full_ = static_cast<unsigned>(rate * kHangSeconds);
  step_ = 1.0f / (rate * kRampSeconds);
  if (!enabled_) {
    gain_ = 1.0f;
  }
}

void Squelch::set_level(bool enabled, float open_dbfs) {
  const bool was = enabled_;
  enabled_ = enabled;
  open_dbfs_ = open_dbfs;
  close_dbfs_ = open_dbfs - kHysteresisDb;
  if (!enabled_) {
    open_ = true;
    gain_ = 1.0f;
    return;
  }
  if (!was) {
    open_ = false;
    gain_ = 0.0f;
    hang_ = 0;
  }
}

bool Squelch::apply(float *audio, std::size_t count, float channel_dbfs) {
  if (!enabled_ || audio == nullptr || count == 0) {
    open_ = true;
    return true;
  }
  if (channel_dbfs >= open_dbfs_) {
    open_ = true;
    hang_ = hang_full_;
  }
  const bool falling = channel_dbfs < close_dbfs_;
  for (std::size_t i = 0; i < count; ++i) {
    if (falling) {
      if (hang_ > 0) {
        --hang_;
      } else {
        open_ = false;
      }
    }
    if (open_) {
      gain_ = std::min(1.0f, gain_ + step_);
    } else {
      gain_ = std::max(0.0f, gain_ - step_);
    }
    audio[i] *= gain_;
  }
  return open_;
}

} // namespace vocatron
