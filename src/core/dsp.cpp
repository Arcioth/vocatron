// Wide FM demodulator.
//
// FM stores the sound in how fast the radio wave's phase spins. A louder
// or higher note makes that spin speed up and slow down. Demodulation
// measures the phase change from one sample to the next, and that change
// is the audio.
//
// Broadcast FM also boosts the treble before transmission. The 50 µs
// de-emphasis filter cuts the treble back down. Europe and Türkiye use
// 50 µs (the US uses 75 µs).

#include "dsp.hpp"

#include "rds.hpp"

#include <complex>

#include <liquid/liquid.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vocatron {
namespace {

constexpr unsigned kChannelDecim = 10; // 2.4 MS/s → 240 kHz
constexpr unsigned kAudioDecim = 5;    // 240 kHz → 48 kHz
constexpr unsigned kAmDecim = 50;      // 2.4 MS/s → 48 kHz
constexpr unsigned kAmTaps = 1201;
constexpr float kAmCutoffHz = 3500.0f;
constexpr float kAmDcR = 0.995f;
// Slow on purpose. A fast volume control turns the gaps between words
// up until they are as loud as the voice.
constexpr float kAmAgcTau = 0.30f;
constexpr float kAmVoiceHz = 2700.0f;
// 2.4 MS/s → 48 kHz. 8 kHz keeps a ±5 kHz marine wiggle and its voice,
// and is down before the next marine channel 25 kHz away.
constexpr unsigned kNfmDecim = 50;
constexpr unsigned kNfmTaps = 2001;
constexpr float kNfmCutoffHz = 8000.0f;
constexpr float kNfmDeviationHz = 5000.0f;
constexpr float kNfmDeemphTau = 750e-6f;
constexpr float kNfmVoiceHz = 3400.0f;
// About 150 Hz. Drops a tuning offset without eating the voice.
constexpr float kNfmDcR = 0.98f;
constexpr std::uint32_t kExpectedRate = 2'400'000;
constexpr float kDeemphTau = 50e-6f;
// Wide FM channel filter: passes about ±80 kHz, gone by ±110 kHz.
constexpr unsigned kWfmChannelTaps = 301;
constexpr float kWfmChannelCutoffHz = 95'000.0f;
// Wide FM audio filter: passes 15 kHz, the 19 kHz pilot is 60 dB down.
constexpr unsigned kWfmAudioTaps = 241;
constexpr float kWfmAudioCutoffHz = 17'000.0f;
// Noise gauge: the demodulated signal above 75 kHz carries no program.
constexpr float kWfmNoiseHz = 75'000.0f;
// Noise gauge readings (dB) for a clean and a buried station. On generated
// stations the gauge read about -20 dB at 0 dB signal-to-noise, -24 at 5,
// -30 at 10, and -39 at 20, whatever the station's strength. On real
// Istanbul stations at the desk (2026-10-11): TRT FM -31.7 and POWER FM
// -27.3 sounded clean; METEOROLOJI and VIRGIN at -19.6 were half noise.
// So 1 is "well above FM's noise cliff" and 0 is "at or below it".
constexpr float kWfmNoiseCleanDb = -32.0f;
constexpr float kWfmNoiseBadDb = -18.0f;
// The treble cut stays off above this clarity and closes in below it.
constexpr float kHicutFromClarity = 0.5f;
// One block is 10 ms, so this smooths over about half a second.
constexpr float kNoiseSmoothing = 0.02f;
constexpr unsigned kNoiseSkipBlocks = 2;
constexpr float kHicutLowHz = 3000.0f;
constexpr float kHicutHighHz = 15000.0f;
constexpr float kAudioRateHz = 48000.0f;
constexpr unsigned kBlockIn = 24000; // divisible by 10*5 and by 50
// Speaker gains. See play_gain_for_mode in dsp.hpp for where they come from.
constexpr float kAmPlayGain = 0.05f;
constexpr float kNfmPlayGain = 1.0f;
constexpr float kWfmPlayGain = 0.25f;

int8_t as_int8(std::uint8_t byte) {
  return byte < 128 ? static_cast<int8_t>(byte) : static_cast<int8_t>(static_cast<int>(byte) - 256);
}

// Mean power of the filtered channel, in dB relative to a full-scale wave.
// The squelch reads this. Speaker loudness is the wrong clue: empty FM is hiss.
struct ChannelMeter {
  double acc = 0.0;
  std::uint64_t count = 0;
  // The channel filter can make a full-scale wave look huge. This is its
  // amplitude gain, so the meter still reads 0 dB for a full-scale input.
  double gain = 1.0;
  float dbfs = -200.0f;

  void add(const liquid_float_complex *samples, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      const float re = samples[i].real();
      const float im = samples[i].imag();
      acc += static_cast<double>(re) * re + static_cast<double>(im) * im;
    }
    count += n;
  }

  void commit() {
    if (count == 0) {
      return;
    }
    const double g = gain > 0.0 ? gain : 1.0;
    const double mean = acc / static_cast<double>(count) / (g * g);
    dbfs = mean > 1.0e-20 ? static_cast<float>(10.0 * std::log10(mean)) : -200.0f;
    acc = 0.0;
    count = 0;
  }
};

// Kaiser low-pass taps scaled so a steady (DC) input comes out `dc_gain`
// times larger. Empty when liquid could not design them.
std::vector<float> lowpass_taps(unsigned count, float cutoff_hz, float rate_hz, float dc_gain) {
  std::vector<float> taps(count);
  if (liquid_firdes_kaiser(count, cutoff_hz / rate_hz, 60.0f, 0.0f, taps.data()) != 0) {
    return {};
  }
  double sum = 0.0;
  for (float tap : taps) {
    sum += tap;
  }
  if (sum <= 0.0) {
    return {};
  }
  const auto scale = static_cast<float>(static_cast<double>(dc_gain) / sum);
  for (float &tap : taps) {
    tap *= scale;
  }
  return taps;
}

class Wfm {
public:
  Wfm(std::uint32_t sample_rate_hz, float shift_hz) : rate_(sample_rate_hz) {
    nco_ = nco_crcf_create(LIQUID_VCO);
    const float omega = 2.0f * 3.14159265f * shift_hz / static_cast<float>(sample_rate_hz);
    nco_crcf_set_frequency(nco_, omega);
    const auto rate = static_cast<float>(sample_rate_hz);
    const float channel_rate = rate / static_cast<float>(kChannelDecim);
    // Keep about ±80 kHz around the station, gone by ±110 kHz, then keep 1
    // of every 10 samples. Wider lets in more hiss from beside the station.
    std::vector<float> channel_taps =
        lowpass_taps(kWfmChannelTaps, kWfmChannelCutoffHz, rate, static_cast<float>(kChannelDecim));
    if (!channel_taps.empty()) {
      channel_ = firdecim_crcf_create(kChannelDecim, channel_taps.data(), kWfmChannelTaps);
    }
    meter_.gain = static_cast<double>(kChannelDecim);
    // kf scales the phase step. 1 means "radians per sample".
    dem_ = freqdem_create(1.0f);
    // Broadcast audio stops at 15 kHz. Everything above it is hiss, plus the
    // 19 kHz stereo pilot tone, so this cuts there and keeps 1 of every 5.
    std::vector<float> audio_taps = lowpass_taps(kWfmAudioTaps, kWfmAudioCutoffHz, channel_rate,
                                                 static_cast<float>(kAudioDecim));
    if (!audio_taps.empty()) {
      audio_ = firdecim_rrrf_create(kAudioDecim, audio_taps.data(), kWfmAudioTaps);
    }
    // Nothing a station sends lives above 75 kHz in the demodulated signal,
    // so whatever is there is noise. Its strength says how clean the station is.
    noise_hp_ = iirfilt_rrrf_create_prototype(LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_HIGHPASS,
                                              LIQUID_IIRDES_SOS, 6, kWfmNoiseHz / channel_rate,
                                              0.0f, 1.0f, 60.0f);
    deemph_a_ = 1.0f - std::exp(-1.0f / (channel_rate * kDeemphTau));
    // One USB chunk is 256 KiB. Keep room for that plus one leftover block
    // so the live path does not allocate while audio is playing.
    scratch_.reserve((1u << 18) + kBlockIn * 2);
  }

  ~Wfm() {
    if (nco_ != nullptr) {
      nco_crcf_destroy(nco_);
    }
    if (channel_ != nullptr) {
      firdecim_crcf_destroy(channel_);
    }
    if (dem_ != nullptr) {
      freqdem_destroy(dem_);
    }
    if (audio_ != nullptr) {
      firdecim_rrrf_destroy(audio_);
    }
    if (noise_hp_ != nullptr) {
      iirfilt_rrrf_destroy(noise_hp_);
    }
  }

  Wfm(const Wfm &) = delete;
  Wfm &operator=(const Wfm &) = delete;

  bool ok() const {
    return nco_ != nullptr && channel_ != nullptr && dem_ != nullptr && audio_ != nullptr &&
           noise_hp_ != nullptr && rate_ == kExpectedRate;
  }

  float channel_dbfs() const { return meter_.dbfs; }
  float noise_db() const { return noise_db_; }
  float clarity() const { return clarity_; }
  const RdsDecoder &rds() const { return rds_; }

  // Returns how many audio samples were written. A short `dst_cap` stops
  // early and leaves the rest in scratch for the next call.
  std::size_t process_into(const std::uint8_t *iq, std::size_t bytes, float *dst,
                           std::size_t dst_cap) {
    if (iq != nullptr && bytes > 0) {
      scratch_.insert(scratch_.end(), iq, iq + bytes);
    }
    const std::size_t block_bytes = kBlockIn * 2;
    constexpr unsigned kAudioOut = kBlockIn / kChannelDecim / kAudioDecim;
    std::size_t off = 0;
    std::size_t written = 0;
    while (scratch_.size() - off >= block_bytes) {
      if (written + kAudioOut > dst_cap) {
        break;
      }
      demod_block(scratch_.data() + off);
      std::memcpy(dst + written, audio_out_.data(), kAudioOut * sizeof(float));
      written += kAudioOut;
      off += block_bytes;
    }
    meter_.commit();
    scratch_.erase(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(off));
    return written;
  }

private:
  void demod_block(const std::uint8_t *iq) {
    for (unsigned n = 0; n < kBlockIn; ++n) {
      const float i = static_cast<float>(as_int8(iq[2 * n])) / 128.0f;
      const float q = static_cast<float>(as_int8(iq[2 * n + 1])) / 128.0f;
      iq_[n] = liquid_float_complex(i, q);
    }
    nco_crcf_mix_block_down(nco_, iq_.data(), mixed_.data(), kBlockIn);

    constexpr unsigned kChannelOut = kBlockIn / kChannelDecim;
    firdecim_crcf_execute_block(channel_, mixed_.data(), kChannelOut, channel_out_.data());
    meter_.add(channel_out_.data(), kChannelOut);

    double noise_sum = 0.0;
    for (unsigned n = 0; n < kChannelOut; ++n) {
      float message = 0.0f;
      freqdem_demodulate(dem_, channel_out_[n], &message);
      float high = 0.0f;
      iirfilt_rrrf_execute(noise_hp_, message, &high);
      noise_sum += static_cast<double>(high) * high;
      mpx_[n] = message;
      // One-pole low-pass. DC gain is 1, treble falls off with a 50 µs time constant.
      deemph_y_ += deemph_a_ * (message - deemph_y_);
      message_[n] = deemph_y_;
    }
    update_clarity(noise_sum / kChannelOut);
    rds_.process(mpx_.data(), kChannelOut);

    constexpr unsigned kAudioOut = kChannelOut / kAudioDecim;
    firdecim_rrrf_execute_block(audio_, message_.data(), kAudioOut, audio_out_.data());

    // Weak-signal treble cut, like a car radio: the noisier the station,
    // the lower this low-pass sits. A clean station passes untouched.
    for (unsigned n = 0; n < kAudioOut; ++n) {
      hicut_y_ += hicut_a_ * (audio_out_[n] - hicut_y_);
      audio_out_[n] = hicut_y_;
    }
  }

  // Smooth the noise reading over about half a second, turn it into a
  // 0..1 clarity, and set the treble cut from it.
  void update_clarity(double noise_power) {
    // The first blocks carry the filters' start-up wobble. Skip them, then
    // take a plain average until there is half a second of history.
    ++noise_blocks_;
    if (noise_blocks_ <= kNoiseSkipBlocks) {
      return;
    }
    const float db =
        noise_power > 1.0e-20 ? static_cast<float>(10.0 * std::log10(noise_power)) : -200.0f;
    const float weight =
        std::max(kNoiseSmoothing, 1.0f / static_cast<float>(noise_blocks_ - kNoiseSkipBlocks));
    noise_db_ += weight * (db - noise_db_);
    float c = (kWfmNoiseBadDb - noise_db_) / (kWfmNoiseBadDb - kWfmNoiseCleanDb);
    c = std::clamp(c, 0.0f, 1.0f);
    clarity_ = c;
    const float h = c / kHicutFromClarity;
    if (h >= 1.0f) {
      hicut_a_ = 1.0f;
      return;
    }
    // From 3 kHz when the station is buried to 15 kHz near the clean end.
    const float cutoff = kHicutLowHz * std::pow(kHicutHighHz / kHicutLowHz, h);
    hicut_a_ = 1.0f - std::exp(-2.0f * 3.14159265f * cutoff / kAudioRateHz);
  }

  std::uint32_t rate_ = 0;
  nco_crcf nco_ = nullptr;
  firdecim_crcf channel_ = nullptr;
  freqdem dem_ = nullptr;
  firdecim_rrrf audio_ = nullptr;
  iirfilt_rrrf noise_hp_ = nullptr;
  float deemph_a_ = 0.0f;
  float deemph_y_ = 0.0f;
  float noise_db_ = -200.0f;
  unsigned noise_blocks_ = 0;
  float clarity_ = 0.0f;
  float hicut_a_ = 1.0f;
  float hicut_y_ = 0.0f;
  std::vector<std::uint8_t> scratch_;
  // Parentheses, not braces: braces would build a one-element vector
  // because std::complex can be constructed from a number.
  std::vector<liquid_float_complex> iq_ = std::vector<liquid_float_complex>(kBlockIn);
  std::vector<liquid_float_complex> mixed_ = std::vector<liquid_float_complex>(kBlockIn);
  std::vector<liquid_float_complex> channel_out_ =
      std::vector<liquid_float_complex>(kBlockIn / kChannelDecim);
  // The demodulated signal before the treble cut: audio, pilot, and RDS.
  std::vector<float> mpx_ = std::vector<float>(kBlockIn / kChannelDecim);
  std::vector<float> message_ = std::vector<float>(kBlockIn / kChannelDecim);
  std::vector<float> audio_out_ = std::vector<float>(kBlockIn / kChannelDecim / kAudioDecim);
  ChannelMeter meter_;
  RdsDecoder rds_;
};

class Am {
public:
  Am(std::uint32_t sample_rate_hz, float shift_hz) : rate_(sample_rate_hz) {
    nco_ = nco_crcf_create(LIQUID_VCO);
    const float omega = 2.0f * 3.14159265f * shift_hz / static_cast<float>(sample_rate_hz);
    nco_crcf_set_frequency(nco_, omega);
    // A narrow low-pass, then keep 1 of every 50 samples. 3.5 kHz
    // keeps airband voice and drops the hiss beside the carrier.
    std::vector<float> taps(kAmTaps);
    const float fc = kAmCutoffHz / static_cast<float>(sample_rate_hz);
    if (liquid_firdes_kaiser(kAmTaps, fc, 60.0f, 0.0f, taps.data()) == 0) {
      channel_ = firdecim_crcf_create(kAmDecim, taps.data(), kAmTaps);
      double sum = 0.0;
      for (float tap : taps) {
        sum += tap;
      }
      meter_.gain = sum;
    }
    const float audio_rate = static_cast<float>(sample_rate_hz) / static_cast<float>(kAmDecim);
    agc_a_ = 1.0f - std::exp(-1.0f / (audio_rate * kAmAgcTau));
    // Airband voice lives under about 2.7 kHz. Higher than that is hiss.
    voice_ = iirfilt_rrrf_create_lowpass(4, kAmVoiceHz / audio_rate);
    // The volume control starts from a guess. The first 50 ms can spike,
    // so those samples stay silent.
    warmup_ = static_cast<unsigned>(audio_rate * 0.05f);
    scratch_.reserve((1u << 18) + kBlockIn * 2);
  }

  ~Am() {
    if (nco_ != nullptr) {
      nco_crcf_destroy(nco_);
    }
    if (channel_ != nullptr) {
      firdecim_crcf_destroy(channel_);
    }
    if (voice_ != nullptr) {
      iirfilt_rrrf_destroy(voice_);
    }
  }

  Am(const Am &) = delete;
  Am &operator=(const Am &) = delete;

  bool ok() const {
    return nco_ != nullptr && channel_ != nullptr && voice_ != nullptr && rate_ == kExpectedRate;
  }

  float channel_dbfs() const { return meter_.dbfs; }

  std::size_t process_into(const std::uint8_t *iq, std::size_t bytes, float *dst,
                           std::size_t dst_cap) {
    if (iq != nullptr && bytes > 0) {
      scratch_.insert(scratch_.end(), iq, iq + bytes);
    }
    const std::size_t block_bytes = kBlockIn * 2;
    constexpr unsigned kAudioOut = kBlockIn / kAmDecim;
    std::size_t off = 0;
    std::size_t written = 0;
    while (scratch_.size() - off >= block_bytes) {
      if (written + kAudioOut > dst_cap) {
        break;
      }
      demod_block(scratch_.data() + off);
      std::memcpy(dst + written, audio_out_.data(), kAudioOut * sizeof(float));
      written += kAudioOut;
      off += block_bytes;
    }
    meter_.commit();
    scratch_.erase(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(off));
    return written;
  }

private:
  void demod_block(const std::uint8_t *iq) {
    for (unsigned n = 0; n < kBlockIn; ++n) {
      const float i = static_cast<float>(as_int8(iq[2 * n])) / 128.0f;
      const float q = static_cast<float>(as_int8(iq[2 * n + 1])) / 128.0f;
      iq_[n] = liquid_float_complex(i, q);
    }
    nco_crcf_mix_block_down(nco_, iq_.data(), mixed_.data(), kBlockIn);

    constexpr unsigned kAudioOut = kBlockIn / kAmDecim;
    firdecim_crcf_execute_block(channel_, mixed_.data(), kAudioOut, channel_out_.data());
    meter_.add(channel_out_.data(), kAudioOut);

    for (unsigned n = 0; n < kAudioOut; ++n) {
      const float mag = std::abs(channel_out_[n]);
      // Subtract the steady carrier. What is left wiggles with the voice.
      const float blocked = mag - dc_x_ + kAmDcR * dc_y_;
      dc_x_ = mag;
      dc_y_ = blocked;
      agc_ += agc_a_ * (std::fabs(blocked) - agc_);
      if (agc_ < 1.0e-4f) {
        agc_ = 1.0e-4f;
      }
      float heard = 0.0f;
      iirfilt_rrrf_execute(voice_, blocked / agc_, &heard);
      if (warmup_ > 0) {
        audio_out_[n] = 0.0f;
        --warmup_;
      } else {
        audio_out_[n] = heard;
      }
    }
  }

  std::uint32_t rate_ = 0;
  nco_crcf nco_ = nullptr;
  firdecim_crcf channel_ = nullptr;
  iirfilt_rrrf voice_ = nullptr;
  float agc_a_ = 0.0f;
  float agc_ = 0.05f;
  float dc_x_ = 0.0f;
  float dc_y_ = 0.0f;
  unsigned warmup_ = 0;
  std::vector<std::uint8_t> scratch_;
  std::vector<liquid_float_complex> iq_ = std::vector<liquid_float_complex>(kBlockIn);
  std::vector<liquid_float_complex> mixed_ = std::vector<liquid_float_complex>(kBlockIn);
  std::vector<liquid_float_complex> channel_out_ =
      std::vector<liquid_float_complex>(kBlockIn / kAmDecim);
  std::vector<float> audio_out_ = std::vector<float>(kBlockIn / kAmDecim);
  ChannelMeter meter_;
};

class Nfm {
public:
  Nfm(std::uint32_t sample_rate_hz, float shift_hz) : rate_(sample_rate_hz) {
    nco_ = nco_crcf_create(LIQUID_VCO);
    const float omega = 2.0f * 3.14159265f * shift_hz / static_cast<float>(sample_rate_hz);
    nco_crcf_set_frequency(nco_, omega);
    std::vector<float> taps(kNfmTaps);
    const float fc = kNfmCutoffHz / static_cast<float>(sample_rate_hz);
    if (liquid_firdes_kaiser(kNfmTaps, fc, 60.0f, 0.0f, taps.data()) == 0) {
      channel_ = firdecim_crcf_create(kNfmDecim, taps.data(), kNfmTaps);
      double sum = 0.0;
      for (float tap : taps) {
        sum += tap;
      }
      meter_.gain = sum;
    }
    const float audio_rate = static_cast<float>(sample_rate_hz) / static_cast<float>(kNfmDecim);
    // freqdem returns hertz / (sample rate * kf). This kf makes ±5 kHz
    // come out as about ±1, before the treble cut.
    dem_ = freqdem_create(kNfmDeviationHz / audio_rate);
    deemph_a_ = 1.0f - std::exp(-1.0f / (audio_rate * kNfmDeemphTau));
    voice_ = iirfilt_rrrf_create_lowpass(4, kNfmVoiceHz / audio_rate);
    warmup_ = static_cast<unsigned>(audio_rate * 0.05f);
    scratch_.reserve((1u << 18) + kBlockIn * 2);
  }

  ~Nfm() {
    if (nco_ != nullptr) {
      nco_crcf_destroy(nco_);
    }
    if (channel_ != nullptr) {
      firdecim_crcf_destroy(channel_);
    }
    if (dem_ != nullptr) {
      freqdem_destroy(dem_);
    }
    if (voice_ != nullptr) {
      iirfilt_rrrf_destroy(voice_);
    }
  }

  Nfm(const Nfm &) = delete;
  Nfm &operator=(const Nfm &) = delete;

  bool ok() const {
    return nco_ != nullptr && channel_ != nullptr && dem_ != nullptr && voice_ != nullptr &&
           rate_ == kExpectedRate;
  }

  float channel_dbfs() const { return meter_.dbfs; }

  std::size_t process_into(const std::uint8_t *iq, std::size_t bytes, float *dst,
                           std::size_t dst_cap) {
    if (iq != nullptr && bytes > 0) {
      scratch_.insert(scratch_.end(), iq, iq + bytes);
    }
    const std::size_t block_bytes = kBlockIn * 2;
    constexpr unsigned kAudioOut = kBlockIn / kNfmDecim;
    std::size_t off = 0;
    std::size_t written = 0;
    while (scratch_.size() - off >= block_bytes) {
      if (written + kAudioOut > dst_cap) {
        break;
      }
      demod_block(scratch_.data() + off);
      std::memcpy(dst + written, audio_out_.data(), kAudioOut * sizeof(float));
      written += kAudioOut;
      off += block_bytes;
    }
    meter_.commit();
    scratch_.erase(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(off));
    return written;
  }

private:
  void demod_block(const std::uint8_t *iq) {
    for (unsigned n = 0; n < kBlockIn; ++n) {
      const float i = static_cast<float>(as_int8(iq[2 * n])) / 128.0f;
      const float q = static_cast<float>(as_int8(iq[2 * n + 1])) / 128.0f;
      iq_[n] = liquid_float_complex(i, q);
    }
    nco_crcf_mix_block_down(nco_, iq_.data(), mixed_.data(), kBlockIn);

    constexpr unsigned kAudioOut = kBlockIn / kNfmDecim;
    firdecim_crcf_execute_block(channel_, mixed_.data(), kAudioOut, channel_out_.data());
    meter_.add(channel_out_.data(), kAudioOut);

    for (unsigned n = 0; n < kAudioOut; ++n) {
      float message = 0.0f;
      freqdem_demodulate(dem_, channel_out_[n], &message);
      // One-pole low-pass. DC gain is 1. Treble falls off with 750 µs,
      // which is the cut handheld and marine radios expect.
      deemph_y_ += deemph_a_ * (message - deemph_y_);
      float voiced = 0.0f;
      iirfilt_rrrf_execute(voice_, deemph_y_, &voiced);
      const float blocked = voiced - dc_x_ + kNfmDcR * dc_y_;
      dc_x_ = voiced;
      dc_y_ = blocked;
      if (warmup_ > 0) {
        audio_out_[n] = 0.0f;
        --warmup_;
      } else {
        audio_out_[n] = blocked;
      }
    }
  }

  std::uint32_t rate_ = 0;
  nco_crcf nco_ = nullptr;
  firdecim_crcf channel_ = nullptr;
  freqdem dem_ = nullptr;
  iirfilt_rrrf voice_ = nullptr;
  float deemph_a_ = 0.0f;
  float deemph_y_ = 0.0f;
  float dc_x_ = 0.0f;
  float dc_y_ = 0.0f;
  unsigned warmup_ = 0;
  std::vector<std::uint8_t> scratch_;
  std::vector<liquid_float_complex> iq_ = std::vector<liquid_float_complex>(kBlockIn);
  std::vector<liquid_float_complex> mixed_ = std::vector<liquid_float_complex>(kBlockIn);
  std::vector<liquid_float_complex> channel_out_ =
      std::vector<liquid_float_complex>(kBlockIn / kNfmDecim);
  std::vector<float> audio_out_ = std::vector<float>(kBlockIn / kNfmDecim);
  ChannelMeter meter_;
};

void write_le16(std::uint8_t *p, unsigned value) {
  p[0] = static_cast<std::uint8_t>(value);
  p[1] = static_cast<std::uint8_t>(value >> 8);
}

void write_le32(std::uint8_t *p, unsigned value) {
  p[0] = static_cast<std::uint8_t>(value);
  p[1] = static_cast<std::uint8_t>(value >> 8);
  p[2] = static_cast<std::uint8_t>(value >> 16);
  p[3] = static_cast<std::uint8_t>(value >> 24);
}

} // namespace

struct WfmStream::Impl {
  Wfm demod;
  Impl(std::uint32_t rate, float shift) : demod(rate, shift) {}
};

WfmStream::WfmStream(std::uint32_t sample_rate_hz, float shift_hz)
    : impl_(std::make_unique<Impl>(sample_rate_hz, shift_hz)) {}

WfmStream::~WfmStream() = default;

bool WfmStream::ok() const { return impl_ != nullptr && impl_->demod.ok(); }

std::size_t WfmStream::process(const std::uint8_t *iq, std::size_t bytes, float *dst,
                               std::size_t dst_cap) {
  if (impl_ == nullptr || dst == nullptr || dst_cap == 0) {
    return 0;
  }
  return impl_->demod.process_into(iq, bytes, dst, dst_cap);
}

float WfmStream::channel_dbfs() const {
  return impl_ != nullptr ? impl_->demod.channel_dbfs() : -200.0f;
}

float WfmStream::noise_db() const { return impl_ != nullptr ? impl_->demod.noise_db() : 0.0f; }

float WfmStream::clarity() const { return impl_ != nullptr ? impl_->demod.clarity() : 0.0f; }

std::string WfmStream::rds_name() const {
  return impl_ != nullptr ? impl_->demod.rds().name() : std::string();
}

std::uint64_t WfmStream::rds_name_packed() const {
  return impl_ != nullptr ? impl_->demod.rds().name_packed() : 0;
}

std::uint16_t WfmStream::rds_pi() const { return impl_ != nullptr ? impl_->demod.rds().pi() : 0; }

struct AmStream::Impl {
  Am demod;
  Impl(std::uint32_t rate, float shift) : demod(rate, shift) {}
};

AmStream::AmStream(std::uint32_t sample_rate_hz, float shift_hz)
    : impl_(std::make_unique<Impl>(sample_rate_hz, shift_hz)) {}

AmStream::~AmStream() = default;

bool AmStream::ok() const { return impl_ != nullptr && impl_->demod.ok(); }

std::size_t AmStream::process(const std::uint8_t *iq, std::size_t bytes, float *dst,
                              std::size_t dst_cap) {
  if (impl_ == nullptr || dst == nullptr || dst_cap == 0) {
    return 0;
  }
  return impl_->demod.process_into(iq, bytes, dst, dst_cap);
}

float AmStream::channel_dbfs() const {
  return impl_ != nullptr ? impl_->demod.channel_dbfs() : -200.0f;
}

struct NfmStream::Impl {
  Nfm demod;
  Impl(std::uint32_t rate, float shift) : demod(rate, shift) {}
};

NfmStream::NfmStream(std::uint32_t sample_rate_hz, float shift_hz)
    : impl_(std::make_unique<Impl>(sample_rate_hz, shift_hz)) {}

NfmStream::~NfmStream() = default;

bool NfmStream::ok() const { return impl_ != nullptr && impl_->demod.ok(); }

std::size_t NfmStream::process(const std::uint8_t *iq, std::size_t bytes, float *dst,
                               std::size_t dst_cap) {
  if (impl_ == nullptr || dst == nullptr || dst_cap == 0) {
    return 0;
  }
  return impl_->demod.process_into(iq, bytes, dst, dst_cap);
}

float NfmStream::channel_dbfs() const {
  return impl_ != nullptr ? impl_->demod.channel_dbfs() : -200.0f;
}

WfmResult demod_wfm(const std::uint8_t *iq, std::size_t bytes, std::uint32_t sample_rate_hz,
                    float shift_hz) {
  WfmResult result;
  if (sample_rate_hz != kExpectedRate || iq == nullptr || bytes < kBlockIn * 2) {
    return result;
  }
  WfmStream stream(sample_rate_hz, shift_hz);
  if (!stream.ok()) {
    return result;
  }
  const std::size_t samples_in = bytes / 2;
  const std::size_t audio_guess = samples_in / (kChannelDecim * kAudioDecim);
  result.audio.reserve(audio_guess);
  // 256 KiB chunks, the same size the live listener pulls off the ring.
  std::vector<float> block(8192);
  const std::size_t even = bytes - (bytes % 2);
  std::size_t off = 0;
  while (off < even) {
    std::size_t n = even - off;
    if (n > (1u << 18)) {
      n = 1u << 18;
    }
    const std::size_t got = stream.process(iq + off, n, block.data(), block.size());
    result.audio.insert(result.audio.end(), block.begin(),
                        block.begin() + static_cast<std::ptrdiff_t>(got));
    off += n;
  }
  result.clarity = stream.clarity();
  result.rds_name = stream.rds_name();
  result.rds_pi = stream.rds_pi();
  result.channel_dbfs = stream.channel_dbfs();
  result.ok = !result.audio.empty();
  return result;
}

WfmResult demod_am(const std::uint8_t *iq, std::size_t bytes, std::uint32_t sample_rate_hz,
                   float shift_hz) {
  WfmResult result;
  if (sample_rate_hz != kExpectedRate || iq == nullptr || bytes < kBlockIn * 2) {
    return result;
  }
  AmStream stream(sample_rate_hz, shift_hz);
  if (!stream.ok()) {
    return result;
  }
  const std::size_t samples_in = bytes / 2;
  result.audio.reserve(samples_in / kAmDecim);
  std::vector<float> block(8192);
  const std::size_t even = bytes - (bytes % 2);
  std::size_t off = 0;
  while (off < even) {
    std::size_t n = even - off;
    if (n > (1u << 18)) {
      n = 1u << 18;
    }
    const std::size_t got = stream.process(iq + off, n, block.data(), block.size());
    result.audio.insert(result.audio.end(), block.begin(),
                        block.begin() + static_cast<std::ptrdiff_t>(got));
    off += n;
  }
  result.channel_dbfs = stream.channel_dbfs();
  result.ok = !result.audio.empty();
  return result;
}

WfmResult demod_nfm(const std::uint8_t *iq, std::size_t bytes, std::uint32_t sample_rate_hz,
                    float shift_hz) {
  WfmResult result;
  if (sample_rate_hz != kExpectedRate || iq == nullptr || bytes < kBlockIn * 2) {
    return result;
  }
  NfmStream stream(sample_rate_hz, shift_hz);
  if (!stream.ok()) {
    return result;
  }
  const std::size_t samples_in = bytes / 2;
  result.audio.reserve(samples_in / kNfmDecim);
  std::vector<float> block(8192);
  const std::size_t even = bytes - (bytes % 2);
  std::size_t off = 0;
  while (off < even) {
    std::size_t n = even - off;
    if (n > (1u << 18)) {
      n = 1u << 18;
    }
    const std::size_t got = stream.process(iq + off, n, block.data(), block.size());
    result.audio.insert(result.audio.end(), block.begin(),
                        block.begin() + static_cast<std::ptrdiff_t>(got));
    off += n;
  }
  result.channel_dbfs = stream.channel_dbfs();
  result.ok = !result.audio.empty();
  return result;
}

float play_gain_for_mode(const char *mode) {
  if (mode != nullptr && std::strcmp(mode, "nfm") == 0) {
    return kNfmPlayGain;
  }
  if (mode != nullptr && std::strcmp(mode, "wfm") == 0) {
    return kWfmPlayGain;
  }
  return kAmPlayGain;
}

bool write_wav_s16(const char *path, const float *audio, std::size_t count, std::uint32_t rate_hz,
                   float peak_target) {
  if (path == nullptr || audio == nullptr || count == 0 || rate_hz == 0) {
    return false;
  }
  float peak = 0.0f;
  for (std::size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::fabs(audio[i]));
  }
  float scale = 32767.0f;
  if (peak_target > 0.0f) {
    scale = peak > 1e-8f ? (peak_target * 32767.0f / peak) : 0.0f;
  }

  FILE *file = std::fopen(path, "wb");
  if (file == nullptr) {
    return false;
  }
  const unsigned data_bytes = static_cast<unsigned>(count * 2);
  std::uint8_t header[44];
  std::memcpy(header, "RIFF", 4);
  write_le32(header + 4, 36u + data_bytes);
  std::memcpy(header + 8, "WAVEfmt ", 8);
  write_le32(header + 16, 16);
  write_le16(header + 20, 1); // PCM
  write_le16(header + 22, 1); // mono
  write_le32(header + 24, rate_hz);
  write_le32(header + 28, rate_hz * 2);
  write_le16(header + 32, 2);
  write_le16(header + 34, 16);
  std::memcpy(header + 36, "data", 4);
  write_le32(header + 40, data_bytes);
  const bool header_ok = std::fwrite(header, 1, sizeof header, file) == sizeof header;

  std::vector<std::int16_t> pcm(count);
  for (std::size_t i = 0; i < count; ++i) {
    const float scaled = audio[i] * scale;
    const float clipped = std::clamp(scaled, -32767.0f, 32767.0f);
    pcm[i] = static_cast<std::int16_t>(std::lrint(clipped));
  }
  const bool body_ok = std::fwrite(pcm.data(), sizeof(std::int16_t), count, file) == count;
  std::fclose(file);
  return header_ok && body_ok;
}

} // namespace vocatron
