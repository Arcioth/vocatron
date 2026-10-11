#pragma once

// RDS: the station's name, sent as slow data inside an FM broadcast.
//
// Next to the music, most FM stations send 1187.5 bits per second on a
// quiet 57 kHz tone in the demodulated signal. Those bits come in groups
// of four 26-bit blocks, each with a 10-bit checksum. Group type 0A
// carries the 8-letter station name (PS), two letters at a time, and every
// block A carries the station's 16-bit code (PI).
//
// Feed the demodulated signal (before de-emphasis) at 240 kHz. Nothing is
// allocated after construction, so this can run on the DSP thread.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <liquid/liquid.h>

namespace vocatron {

class RdsDecoder {
public:
  explicit RdsDecoder(float mpx_rate_hz = 240'000.0f);
  ~RdsDecoder();

  RdsDecoder(const RdsDecoder &) = delete;
  RdsDecoder &operator=(const RdsDecoder &) = delete;

  bool ok() const;
  void process(const float *mpx, std::size_t count);

  // Station name once all four parts arrived, trailing spaces removed.
  // Empty until then. Letters RDS sends outside plain ASCII become '?'.
  std::string name() const;
  // The same 8 letters packed into one number (first letter in the low
  // byte), 0 until complete. Lets another thread read it from an atomic.
  std::uint64_t name_packed() const;
  // Station code from block A. 0 until a block A checks out.
  std::uint16_t pi() const { return pi_; }
  // Blocks that passed their checksum. A rough "RDS is coming in" gauge.
  std::uint64_t good_blocks() const { return good_blocks_; }

private:
  void on_symbol(float value);
  void on_bit(unsigned bit);
  void on_block(unsigned index, std::uint16_t data);

  // 57 kHz down to 0 Hz, keep ±2.4 kHz, then 4 samples per RDS symbol.
  nco_crcf nco_ = nullptr;
  firdecim_crcf decim_ = nullptr;
  agc_crcf agc_ = nullptr;
  resamp_crcf resamp_ = nullptr;
  symsync_crcf sync_ = nullptr;
  unsigned decim_factor_ = 0;
  std::vector<std::complex<float>> in_;
  unsigned in_fill_ = 0;
  std::vector<std::complex<float>> resampled_;
  std::vector<std::complex<float>> symbols_;

  // Carrier phase tracking (Costas loop for two-phase data).
  float phase_ = 0.0f;
  float freq_ = 0.0f;

  // Each bit is two opposite symbols. Track which pairing is the real one.
  float prev_symbol_ = 0.0f;
  unsigned symbol_count_ = 0;
  float pair_strength_[2] = {0.0f, 0.0f};
  unsigned prev_bit_ = 0;

  // Block finding.
  std::uint32_t shift_ = 0;
  unsigned bits_seen_ = 0;
  bool synced_ = false;
  // One block checked out; waiting to see if the next one does too.
  bool trial_ = false;
  unsigned bits_into_block_ = 0;
  unsigned next_block_ = 0;
  unsigned bad_blocks_ = 0;
  std::uint16_t blocks_[4] = {0, 0, 0, 0};
  bool block_ok_[4] = {false, false, false, false};

  char name_[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  bool part_seen_[4] = {false, false, false, false};
  // Name parts and the station code only count once they arrive twice.
  std::uint32_t part_candidate_[4] = {0x10000u, 0x10000u, 0x10000u, 0x10000u};
  std::uint16_t pi_candidate_ = 0;
  std::uint16_t pi_ = 0;
  std::uint64_t good_blocks_ = 0;
};

// The 10-bit checksum math, exposed so tests can build RDS on purpose.
// Remainder of a 26-bit block divided by the RDS generator polynomial.
std::uint16_t rds_syndrome(std::uint32_t block26);
// Offset words that mark blocks A, B, C, D (and C' at index 4).
std::uint16_t rds_offset(unsigned index);
// 26-bit block: 16 data bits then checksum XOR offset.
std::uint32_t rds_make_block(std::uint16_t data, unsigned index);

} // namespace vocatron
