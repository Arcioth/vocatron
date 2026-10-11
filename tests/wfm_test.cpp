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

} // namespace

TEST_CASE("wide FM demod recovers a 1 kHz tone") {
  constexpr std::uint32_t kRate = 2'400'000;
  constexpr float kShift = 250'000.0f;
  constexpr float kAudio = 1000.0f;
  constexpr float kDeviation = 75'000.0f;
  constexpr std::size_t kSamples = kRate / 2; // 0.5 s

  std::vector<std::uint8_t> iq(kSamples * 2);
  double phase = 0.0;
  double audio_phase = 0.0;
  const double audio_step = 2.0 * 3.14159265358979323846 * kAudio / kRate;
  const double carrier_step = 2.0 * 3.14159265358979323846 * kShift / kRate;
  for (std::size_t n = 0; n < kSamples; ++n) {
    const double inst =
        carrier_step + (kDeviation / kRate) * 2.0 * 3.14159265358979323846 * std::sin(audio_phase);
    phase += inst;
    audio_phase += audio_step;
    const int i = static_cast<int>(std::lround(0.5 * std::cos(phase) * 128.0));
    const int q = static_cast<int>(std::lround(0.5 * std::sin(phase) * 128.0));
    iq[2 * n] = static_cast<std::uint8_t>(static_cast<std::int8_t>(i));
    iq[2 * n + 1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(q));
  }

  const vocatron::WfmResult result = vocatron::demod_wfm(iq.data(), iq.size(), kRate, kShift);
  REQUIRE(result.ok);
  // Skip the filter warmup. 48 kHz, so 2400 samples is 50 ms.
  REQUIRE(result.audio.size() > 4800);
  const float *audio = result.audio.data() + 2400;
  const std::size_t n = result.audio.size() - 2400;
  const float at_tone = goertzel(audio, n, kAudio, 48000.0f);
  const float at_low = goertzel(audio, n, 300.0f, 48000.0f);
  const float at_high = goertzel(audio, n, 3000.0f, 48000.0f);
  CHECK(at_tone > at_low * 20.0f);
  CHECK(at_tone > at_high * 20.0f);
}

TEST_CASE("wide FM demod keeps the tone across uneven chunks") {
  constexpr std::uint32_t kRate = 2'400'000;
  constexpr float kShift = 250'000.0f;
  constexpr float kAudio = 1000.0f;
  constexpr float kDeviation = 75'000.0f;
  constexpr std::size_t kSamples = kRate / 2;

  std::vector<std::uint8_t> iq(kSamples * 2);
  double phase = 0.0;
  double audio_phase = 0.0;
  const double audio_step = 2.0 * 3.14159265358979323846 * kAudio / kRate;
  const double carrier_step = 2.0 * 3.14159265358979323846 * kShift / kRate;
  for (std::size_t n = 0; n < kSamples; ++n) {
    const double inst =
        carrier_step + (kDeviation / kRate) * 2.0 * 3.14159265358979323846 * std::sin(audio_phase);
    phase += inst;
    audio_phase += audio_step;
    const int i = static_cast<int>(std::lround(0.5 * std::cos(phase) * 128.0));
    const int q = static_cast<int>(std::lround(0.5 * std::sin(phase) * 128.0));
    iq[2 * n] = static_cast<std::uint8_t>(static_cast<std::int8_t>(i));
    iq[2 * n + 1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(q));
  }

  vocatron::WfmStream stream(kRate, kShift);
  REQUIRE(stream.ok());
  std::vector<float> audio;
  audio.reserve(kSamples / 50);
  std::vector<float> block(8192);
  // Alternate a short piece and a full USB-sized piece, the way the live
  // reader sees whatever the queue happens to have.
  std::size_t off = 0;
  bool big = false;
  while (off + 1 < iq.size()) {
    std::size_t n = big ? (1u << 18) : 1000;
    big = !big;
    if (off + n > iq.size()) {
      n = iq.size() - off;
    }
    if (n % 2 != 0) {
      --n;
    }
    const std::size_t got = stream.process(iq.data() + off, n, block.data(), block.size());
    audio.insert(audio.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(got));
    off += n;
  }

  REQUIRE(audio.size() > 4800);
  const float *tail = audio.data() + 2400;
  const std::size_t n = audio.size() - 2400;
  const float at_tone = goertzel(tail, n, kAudio, 48000.0f);
  const float at_low = goertzel(tail, n, 300.0f, 48000.0f);
  const float at_high = goertzel(tail, n, 3000.0f, 48000.0f);
  CHECK(at_tone > at_low * 20.0f);
  CHECK(at_tone > at_high * 20.0f);
}
