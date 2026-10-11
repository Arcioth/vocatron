#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vocatron {

struct WfmResult {
  bool ok = false;
  // 48 kHz mono audio. Empty when ok is false.
  std::vector<float> audio;
  // Strength of the filtered channel. 0 dB is a full-scale wave.
  float channel_dbfs = -200.0f;
  // Wide FM only: 0 (buried) to 1 (clean), and the RDS name if one arrived.
  float clarity = 0.0f;
  std::string rds_name;
  std::uint16_t rds_pi = 0;
};

// One wide-FM demodulator kept alive for a whole listen.
//
// The filters remember the last samples they saw. Building a new
// demodulator for every block would click. Feed it I/Q bytes as they
// arrive; it appends 48 kHz audio. `shift_hz` is how far the channel
// sits above the radio's tuned frequency (250 kHz for our captures).
class WfmStream {
public:
  WfmStream(std::uint32_t sample_rate_hz, float shift_hz);
  ~WfmStream();

  WfmStream(const WfmStream &) = delete;
  WfmStream &operator=(const WfmStream &) = delete;

  bool ok() const;
  // Strength of the filtered channel, from the last process() call.
  float channel_dbfs() const;
  // How much noise rides on the station, smoothed over about half a second.
  // Measured above 75 kHz in the demodulated signal, where no program lives,
  // so it does not depend on the gain settings. Lower is cleaner.
  float noise_db() const;
  // The same reading as 0 (buried in noise) to 1 (clean). Below about
  // 0.6 the weak-signal treble cut is active.
  float clarity() const;
  // Station name from RDS, empty until all of it arrived (see rds.hpp).
  std::string rds_name() const;
  // The same 8 letters packed into one number, for passing between threads.
  std::uint64_t rds_name_packed() const;
  // RDS station code, 0 until seen.
  std::uint16_t rds_pi() const;

  // Write produced audio into `dst`. Returns how many samples were written.
  // `dst_cap` must be at least one sample per 50 input samples, plus 480.
  std::size_t process(const std::uint8_t *iq, std::size_t bytes, float *dst, std::size_t dst_cap);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Turn a whole capture into wide-FM audio. Same chain as WfmStream.
// Europe/Türkiye de-emphasis (50 µs).
WfmResult demod_wfm(const std::uint8_t *iq, std::size_t bytes, std::uint32_t sample_rate_hz,
                    float shift_hz);

// One AM demodulator kept alive for a whole listen.
//
// AM stores the sound in the strength of the wave. After the channel is
// moved to the middle and narrowed, the strength of each sample is the
// audio, riding on a steady carrier. The carrier is removed and a slow
// volume control keeps a quiet station audible.
class AmStream {
public:
  AmStream(std::uint32_t sample_rate_hz, float shift_hz);
  ~AmStream();

  AmStream(const AmStream &) = delete;
  AmStream &operator=(const AmStream &) = delete;

  bool ok() const;
  float channel_dbfs() const;
  std::size_t process(const std::uint8_t *iq, std::size_t bytes, float *dst, std::size_t dst_cap);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Whole-capture AM demod. Same chain as AmStream. 48 kHz mono.
WfmResult demod_am(const std::uint8_t *iq, std::size_t bytes, std::uint32_t sample_rate_hz,
                   float shift_hz);

// One narrow-FM demodulator kept alive for a whole listen.
//
// Same idea as wide FM: the sound is how fast the wave's phase spins.
// A handheld or a marine radio only wiggles that by a few kHz, so the
// filter is much tighter. ±5 kHz of wiggle comes out near full scale.
// The 750 µs treble cut undoes the treble boost those radios add.
class NfmStream {
public:
  NfmStream(std::uint32_t sample_rate_hz, float shift_hz);
  ~NfmStream();

  NfmStream(const NfmStream &) = delete;
  NfmStream &operator=(const NfmStream &) = delete;

  bool ok() const;
  float channel_dbfs() const;
  std::size_t process(const std::uint8_t *iq, std::size_t bytes, float *dst, std::size_t dst_cap);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Whole-capture narrow-FM demod. Same chain as NfmStream. 48 kHz mono.
WfmResult demod_nfm(const std::uint8_t *iq, std::size_t bytes, std::uint32_t sample_rate_hz,
                    float shift_hz);

// Speaker gain for "wfm", "am", or "nfm", so a normal voice plays at about
// the same loudness in every mode.
//
// The three demodulators come out at very different sizes. AM's volume
// control holds speech near 1.6 RMS. Narrow FM turns 3 kHz of wiggle on a
// 1 kHz voice into only about 0.09 RMS after the treble cut. Wide FM turns
// a 25 kHz program swing into about 0.35 RMS. Each gain brings that to about
// 0.08 RMS at the speaker. Empty-channel hiss stays under full scale in
// every mode. tests/level_test.cpp checks these numbers.
float play_gain_for_mode(const char *mode);

// 16-bit mono WAV.
// `peak_target` 0.4 scales the loudest peak to 40% of full scale.
// `peak_target` 0 means the samples are already in [-1, 1].
bool write_wav_s16(const char *path, const float *audio, std::size_t count, std::uint32_t rate_hz,
                   float peak_target = 0.4f);

} // namespace vocatron
