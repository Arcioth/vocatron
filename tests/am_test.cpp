#include "core/dsp.hpp"

#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

float goertzel(const float *x, std::size_t n, float freq_hz, float rate_hz) {
  const float w = 2.0f * 3.14159265f * freq_hz / rate_hz;
  const float coeff = 2.0f * std::cos(w);
  float s1 = 0.0f;
  float s2 = 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const float s0 = x[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

std::vector<std::uint8_t> am_iq(float audio_hz) {
  constexpr std::uint32_t kRate = 2'400'000;
  constexpr float kShift = 250'000.0f;
  constexpr std::size_t kSamples = kRate / 2;
  std::vector<std::uint8_t> iq(kSamples * 2);
  double phase = 0.0;
  double audio_phase = 0.0;
  const double audio_step = 2.0 * 3.14159265358979323846 * audio_hz / kRate;
  const double carrier_step = 2.0 * 3.14159265358979323846 * kShift / kRate;
  for (std::size_t n = 0; n < kSamples; ++n) {
    const double env = 1.0 + 0.5 * std::sin(audio_phase);
    const double amp = 0.4 * env;
    phase += carrier_step;
    audio_phase += audio_step;
    const int i = static_cast<int>(std::lround(amp * std::cos(phase) * 127.0));
    const int q = static_cast<int>(std::lround(amp * std::sin(phase) * 127.0));
    iq[2 * n] = static_cast<std::uint8_t>(static_cast<std::int8_t>(i));
    iq[2 * n + 1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(q));
  }
  return iq;
}

} // namespace

TEST_CASE("AM demod recovers a 1 kHz tone") {
  constexpr std::uint32_t kRate = 2'400'000;
  constexpr float kShift = 250'000.0f;
  const std::vector<std::uint8_t> iq = am_iq(1000.0f);
  const vocatron::WfmResult result = vocatron::demod_am(iq.data(), iq.size(), kRate, kShift);
  REQUIRE(result.ok);
  // 100 ms at 48 kHz. The volume control needs a moment to settle.
  REQUIRE(result.audio.size() > 9600);
  const float *audio = result.audio.data() + 4800;
  const std::size_t n = result.audio.size() - 4800;
  const float at_tone = goertzel(audio, n, 1000.0f, 48000.0f);
  const float at_low = goertzel(audio, n, 300.0f, 48000.0f);
  const float at_high = goertzel(audio, n, 3000.0f, 48000.0f);
  CHECK(at_tone > at_low * 20.0f);
  CHECK(at_tone > at_high * 20.0f);
}
