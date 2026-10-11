// Speaker loudness per mode.
//
// A normal voice should come out about equally loud in AM, narrow FM, and
// wide FM, and an empty channel's hiss should never clip.

#include "core/dsp.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace {

constexpr std::uint32_t kRate = 2'400'000;
constexpr float kShift = 250'000.0f;
constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

std::uint8_t as_byte(double value) {
  return static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(value * 127.0)));
}

// Half-scale carrier whose frequency wiggles by `deviation_hz` at `audio_hz`.
std::vector<std::uint8_t> fm_iq(float audio_hz, float deviation_hz) {
  std::vector<std::uint8_t> iq(static_cast<std::size_t>(kRate));
  double phase = 0.0;
  double audio_phase = 0.0;
  for (std::size_t n = 0; n < iq.size() / 2; ++n) {
    audio_phase += kTwoPi * audio_hz / kRate;
    phase += kTwoPi * (kShift + deviation_hz * std::sin(audio_phase)) / kRate;
    iq[2 * n] = as_byte(0.5 * std::cos(phase));
    iq[2 * n + 1] = as_byte(0.5 * std::sin(phase));
  }
  return iq;
}

// Carrier whose strength swings by `depth` at `audio_hz`.
std::vector<std::uint8_t> am_iq(float audio_hz, float depth) {
  std::vector<std::uint8_t> iq(static_cast<std::size_t>(kRate));
  double phase = 0.0;
  double audio_phase = 0.0;
  for (std::size_t n = 0; n < iq.size() / 2; ++n) {
    audio_phase += kTwoPi * audio_hz / kRate;
    phase += kTwoPi * kShift / kRate;
    const double amp = 0.4 * (1.0 + depth * std::sin(audio_phase));
    iq[2 * n] = as_byte(amp * std::cos(phase));
    iq[2 * n + 1] = as_byte(amp * std::sin(phase));
  }
  return iq;
}

// No station. Random noise a few counts wide, like an empty channel.
std::vector<std::uint8_t> noise_iq() {
  std::mt19937 rng(1);
  std::normal_distribution<float> dist(0.0f, 6.0f);
  std::vector<std::uint8_t> iq(static_cast<std::size_t>(kRate));
  for (std::uint8_t &byte : iq) {
    const long value = std::clamp(std::lround(dist(rng)), -127L, 127L);
    byte = static_cast<std::uint8_t>(static_cast<std::int8_t>(value));
  }
  return iq;
}

struct Loudness {
  float rms = 0.0f;
  float peak = 0.0f;
};

// Loudness at the speaker, skipping the first 100 ms while filters settle.
Loudness at_speaker(const vocatron::WfmResult &result, const char *mode) {
  const float gain = vocatron::play_gain_for_mode(mode);
  double sum = 0.0;
  float peak = 0.0f;
  std::size_t count = 0;
  for (std::size_t i = 4800; i < result.audio.size(); ++i) {
    const float sample = result.audio[i] * gain;
    sum += static_cast<double>(sample) * sample;
    peak = std::max(peak, std::fabs(sample));
    ++count;
  }
  Loudness out;
  out.rms = count > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : 0.0f;
  out.peak = peak;
  return out;
}

} // namespace

TEST_CASE("a normal voice plays about equally loud in every mode") {
  const std::vector<std::uint8_t> am = am_iq(1000.0f, 0.8f);
  const std::vector<std::uint8_t> nfm = fm_iq(1000.0f, 3000.0f);
  const std::vector<std::uint8_t> wfm = fm_iq(1000.0f, 25'000.0f);
  const Loudness am_out = at_speaker(vocatron::demod_am(am.data(), am.size(), kRate, kShift), "am");
  const Loudness nfm_out =
      at_speaker(vocatron::demod_nfm(nfm.data(), nfm.size(), kRate, kShift), "nfm");
  const Loudness wfm_out =
      at_speaker(vocatron::demod_wfm(wfm.data(), wfm.size(), kRate, kShift), "wfm");

  // About -22 dBFS RMS, give or take 6 dB.
  for (const Loudness &out : {am_out, nfm_out, wfm_out}) {
    CHECK(out.rms > 0.04f);
    CHECK(out.rms < 0.16f);
    CHECK(out.peak < 1.0f);
  }
  // No mode more than 6 dB louder than another.
  const float loudest = std::max({am_out.rms, nfm_out.rms, wfm_out.rms});
  const float quietest = std::min({am_out.rms, nfm_out.rms, wfm_out.rms});
  CHECK(loudest < quietest * 2.0f);
}

TEST_CASE("empty-channel hiss does not clip the speaker") {
  const std::vector<std::uint8_t> iq = noise_iq();
  const Loudness am_out = at_speaker(vocatron::demod_am(iq.data(), iq.size(), kRate, kShift), "am");
  const Loudness nfm_out =
      at_speaker(vocatron::demod_nfm(iq.data(), iq.size(), kRate, kShift), "nfm");
  const Loudness wfm_out =
      at_speaker(vocatron::demod_wfm(iq.data(), iq.size(), kRate, kShift), "wfm");
  CHECK(am_out.peak < 1.0f);
  CHECK(nfm_out.peak < 1.0f);
  CHECK(wfm_out.peak < 1.0f);
}
