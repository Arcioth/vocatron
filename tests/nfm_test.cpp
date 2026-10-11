#include "core/dsp.hpp"

#include <doctest.h>

#include <algorithm>
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

// Constant-strength wave whose frequency wiggles by `deviation_hz` at `audio_hz`.
// The carrier sits 250 kHz above the radio's tune, same as a live capture.
std::vector<std::uint8_t> nfm_iq(float audio_hz, float deviation_hz) {
  constexpr std::uint32_t kRate = 2'400'000;
  constexpr float kShift = 250'000.0f;
  constexpr std::size_t kSamples = kRate / 2;
  std::vector<std::uint8_t> iq(kSamples * 2);
  double phase = 0.0;
  double audio_phase = 0.0;
  const double audio_step = 2.0 * 3.14159265358979323846 * audio_hz / kRate;
  const double carrier_step = 2.0 * 3.14159265358979323846 * kShift / kRate;
  const double two_pi = 2.0 * 3.14159265358979323846;
  for (std::size_t n = 0; n < kSamples; ++n) {
    audio_phase += audio_step;
    const double wiggle = deviation_hz * std::sin(audio_phase);
    phase += carrier_step + two_pi * wiggle / kRate;
    const int i = static_cast<int>(std::lround(0.5 * std::cos(phase) * 127.0));
    const int q = static_cast<int>(std::lround(0.5 * std::sin(phase) * 127.0));
    iq[2 * n] = static_cast<std::uint8_t>(static_cast<std::int8_t>(i));
    iq[2 * n + 1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(q));
  }
  return iq;
}

} // namespace

TEST_CASE("narrow FM demod recovers a 1 kHz tone") {
  constexpr std::uint32_t kRate = 2'400'000;
  constexpr float kShift = 250'000.0f;
  // 3 kHz of wiggle. Marine radios can do 5 kHz, handhelds often 2.5.
  const std::vector<std::uint8_t> iq = nfm_iq(1000.0f, 3000.0f);
  const vocatron::WfmResult result = vocatron::demod_nfm(iq.data(), iq.size(), kRate, kShift);
  REQUIRE(result.ok);
  REQUIRE(result.audio.size() > 20000);
  // 100 ms at 48 kHz. The treble filter and the first samples need to settle.
  const float *audio = result.audio.data() + 4800;
  const std::size_t n = result.audio.size() - 4800;
  float peak = 0.0f;
  for (std::size_t i = 0; i < n; ++i) {
    peak = std::max(peak, std::fabs(audio[i]));
  }
  // ±5 kHz would be about 1 before the treble cut. This tone is smaller,
  // and the cut brings 1 kHz down further. It should still be a real tone.
  CHECK(peak > 0.01f);
  CHECK(peak < 1.5f);
  const float at_tone = goertzel(audio, n, 1000.0f, 48000.0f);
  const float at_low = goertzel(audio, n, 300.0f, 48000.0f);
  const float at_high = goertzel(audio, n, 3000.0f, 48000.0f);
  CHECK(at_tone > at_low * 20.0f);
  CHECK(at_tone > at_high * 20.0f);
  // Amplitude 0.5 is about -6 dBFS when the channel filter passes it whole.
  CHECK(result.channel_dbfs > -18.0f);
  CHECK(result.channel_dbfs < 0.0f);
}

TEST_CASE("narrow FM demod stays quiet on a steady carrier") {
  constexpr std::uint32_t kRate = 2'400'000;
  constexpr float kShift = 250'000.0f;
  const std::vector<std::uint8_t> iq = nfm_iq(0.0f, 0.0f);
  const vocatron::WfmResult result = vocatron::demod_nfm(iq.data(), iq.size(), kRate, kShift);
  REQUIRE(result.ok);
  REQUIRE(result.audio.size() > 4800);
  float peak = 0.0f;
  for (std::size_t i = 4800; i < result.audio.size(); ++i) {
    peak = std::max(peak, std::fabs(result.audio[i]));
  }
  CHECK(peak < 0.02f);
}
