#include "rds.hpp"

#include <algorithm>
#include <cmath>

namespace vocatron {
namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kSubcarrierHz = 57'000.0f;
// RDS symbols: each data bit (1187.5 per second) is two half-bit symbols.
constexpr float kSymbolRateHz = 2375.0f;
constexpr unsigned kSamplesPerSymbol = 4;
// 240 kHz / 25 = 9.6 kHz, then resampled to exactly 4 × 2375 = 9.5 kHz.
constexpr unsigned kDecim = 25;
// Keep ±2.4 kHz of RDS. Gone by ±4 kHz, where the stereo (L−R) sound ends.
constexpr unsigned kDecimTaps = 551;
constexpr float kDecimCutoffHz = 3000.0f;
// RDS generator polynomial x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1.
constexpr std::uint32_t kPoly = 0x5B9;
// Offset words for blocks A, B, C, D, and C'.
constexpr std::uint16_t kOffsets[5] = {0x0FC, 0x198, 0x168, 0x1B4, 0x350};
// Costas loop gains, per symbol.
constexpr float kLoopAlpha = 0.05f;
constexpr float kLoopBeta = 0.0008f;
// Lose block sync after this many failed blocks in a row.
constexpr unsigned kMaxBadBlocks = 10;

char rds_char(unsigned code) {
  return code >= 0x20 && code <= 0x7E ? static_cast<char>(code) : '?';
}

} // namespace

std::uint16_t rds_syndrome(std::uint32_t block26) {
  std::uint32_t reg = block26 & 0x3FFFFFFu;
  for (int bit = 25; bit >= 10; --bit) {
    if ((reg & (1u << bit)) != 0) {
      reg ^= kPoly << (bit - 10);
    }
  }
  return static_cast<std::uint16_t>(reg & 0x3FFu);
}

std::uint16_t rds_offset(unsigned index) { return index < 5 ? kOffsets[index] : 0; }

std::uint32_t rds_make_block(std::uint16_t data, unsigned index) {
  const std::uint32_t shifted = static_cast<std::uint32_t>(data) << 10;
  return shifted | (rds_syndrome(shifted) ^ rds_offset(index));
}

RdsDecoder::RdsDecoder(float mpx_rate_hz) : decim_factor_(kDecim) {
  nco_ = nco_crcf_create(LIQUID_VCO);
  nco_crcf_set_frequency(nco_, kTwoPi * kSubcarrierHz / mpx_rate_hz);

  std::vector<float> taps(kDecimTaps);
  if (liquid_firdes_kaiser(kDecimTaps, kDecimCutoffHz / mpx_rate_hz, 60.0f, 0.0f, taps.data()) ==
      0) {
    decim_ = firdecim_crcf_create(kDecim, taps.data(), kDecimTaps);
  }
  agc_ = agc_crcf_create();
  agc_crcf_set_bandwidth(agc_, 1.0e-3f);

  const float decimated = mpx_rate_hz / static_cast<float>(kDecim);
  const float wanted = kSymbolRateHz * static_cast<float>(kSamplesPerSymbol);
  resamp_ = resamp_crcf_create(wanted / decimated, 13, 0.45f, 60.0f, 32);
  sync_ = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, kSamplesPerSymbol, 3, 0.8f, 32);
  if (sync_ != nullptr) {
    symsync_crcf_set_lf_bw(sync_, 0.02f);
  }
  in_.resize(kDecim);
  resampled_.resize(4);
  symbols_.resize(8);
}

RdsDecoder::~RdsDecoder() {
  if (nco_ != nullptr) {
    nco_crcf_destroy(nco_);
  }
  if (decim_ != nullptr) {
    firdecim_crcf_destroy(decim_);
  }
  if (agc_ != nullptr) {
    agc_crcf_destroy(agc_);
  }
  if (resamp_ != nullptr) {
    resamp_crcf_destroy(resamp_);
  }
  if (sync_ != nullptr) {
    symsync_crcf_destroy(sync_);
  }
}

bool RdsDecoder::ok() const {
  return nco_ != nullptr && decim_ != nullptr && agc_ != nullptr && resamp_ != nullptr &&
         sync_ != nullptr;
}

void RdsDecoder::process(const float *mpx, std::size_t count) {
  if (!ok()) {
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    std::complex<float> mixed;
    nco_crcf_mix_down(nco_, std::complex<float>(mpx[i], 0.0f), &mixed);
    nco_crcf_step(nco_);
    in_[in_fill_++] = mixed;
    if (in_fill_ < decim_factor_) {
      continue;
    }
    in_fill_ = 0;
    std::complex<float> narrow;
    firdecim_crcf_execute(decim_, in_.data(), &narrow);
    std::complex<float> level;
    agc_crcf_execute(agc_, narrow, &level);
    unsigned made = 0;
    resamp_crcf_execute(resamp_, level, resampled_.data(), &made);
    unsigned symbols = 0;
    symsync_crcf_execute(sync_, resampled_.data(), made, symbols_.data(), &symbols);
    for (unsigned s = 0; s < symbols; ++s) {
      // Turn the symbol back by the tracked phase. Two-phase data should
      // then sit on the real axis; whatever is left on the imaginary axis
      // is phase error, which nudges the loop.
      const std::complex<float> turned = symbols_[s] * std::polar(1.0f, -phase_);
      const float error = std::clamp(turned.real() * turned.imag(), -1.0f, 1.0f);
      freq_ += kLoopBeta * error;
      phase_ += freq_ + kLoopAlpha * error;
      phase_ = std::remainder(phase_, kTwoPi);
      on_symbol(turned.real());
    }
  }
}

void RdsDecoder::on_symbol(float value) {
  // A data bit is two opposite symbols. Which pairing is right shows up as
  // the pairing with the bigger difference inside each pair.
  const unsigned parity = symbol_count_ & 1u;
  ++symbol_count_;
  const float diff = prev_symbol_ - value;
  prev_symbol_ = value;
  pair_strength_[parity] = 0.99f * pair_strength_[parity] + 0.01f * std::fabs(diff);
  const unsigned chosen = pair_strength_[1] > pair_strength_[0] ? 1u : 0u;
  if (parity != chosen) {
    return;
  }
  const unsigned bit = diff > 0.0f ? 1u : 0u;
  // RDS sends each bit as "changed" or "same" compared with the last one,
  // which also undoes a flipped carrier phase.
  on_bit(bit ^ prev_bit_);
  prev_bit_ = bit;
}

void RdsDecoder::on_bit(unsigned bit) {
  shift_ = ((shift_ << 1) | bit) & 0x3FFFFFFu;
  if (bits_seen_ < 26) {
    ++bits_seen_;
    return;
  }
  const std::uint16_t syndrome = rds_syndrome(shift_);
  const auto data = static_cast<std::uint16_t>(shift_ >> 10);
  if (!synced_) {
    // A 10-bit checksum also matches random noise about once in 1000
    // tries, so one good block proves nothing. Remember it, and only call
    // it sync when the next block, 26 bits later, checks out too.
    if (trial_) {
      if (++bits_into_block_ < 26) {
        return;
      }
      trial_ = false;
      const unsigned index = next_block_;
      if (syndrome == kOffsets[index] || (index == 2 && syndrome == kOffsets[4])) {
        synced_ = true;
        bad_blocks_ = 0;
        bits_into_block_ = 0;
        next_block_ = (index + 1) % 4;
        block_ok_[index] = true;
        on_block(index, data);
        return;
      }
    }
    for (unsigned index = 0; index < 5; ++index) {
      if (syndrome == kOffsets[index]) {
        const unsigned real_index = index == 4 ? 2 : index;
        trial_ = true;
        bits_into_block_ = 0;
        next_block_ = (real_index + 1) % 4;
        for (bool &ok : block_ok_) {
          ok = false;
        }
        block_ok_[real_index] = true;
        blocks_[real_index] = data;
        break;
      }
    }
    return;
  }
  if (++bits_into_block_ < 26) {
    return;
  }
  bits_into_block_ = 0;
  const unsigned index = next_block_;
  next_block_ = (index + 1) % 4;
  if (index == 0) {
    for (bool &ok : block_ok_) {
      ok = false;
    }
  }
  const bool good = syndrome == kOffsets[index] || (index == 2 && syndrome == kOffsets[4]);
  block_ok_[index] = good;
  if (good) {
    bad_blocks_ = 0;
    on_block(index, data);
  } else if (++bad_blocks_ >= kMaxBadBlocks) {
    synced_ = false;
    bits_into_block_ = 0;
  }
}

void RdsDecoder::on_block(unsigned index, std::uint16_t data) {
  ++good_blocks_;
  blocks_[index] = data;
  if (index == 0) {
    // Same code twice in a row before it is believed.
    if (data == pi_candidate_) {
      pi_ = data;
    }
    pi_candidate_ = data;
  }
  if (index != 3 || !block_ok_[1]) {
    return;
  }
  // Group type 0 (A or B) carries two letters of the name in block D.
  const std::uint16_t b = blocks_[1];
  if ((b >> 12) != 0) {
    return;
  }
  const unsigned part = b & 3u;
  // A pair of letters counts at once when its whole group passed all four
  // checksums. Otherwise it has to arrive the same way twice.
  const bool whole_group = block_ok_[0] && block_ok_[1] && block_ok_[2];
  if (!whole_group && part_candidate_[part] != data) {
    part_candidate_[part] = data;
    return;
  }
  part_candidate_[part] = data;
  name_[part * 2] = rds_char(data >> 8);
  name_[part * 2 + 1] = rds_char(data & 0xFFu);
  part_seen_[part] = true;
}

std::string RdsDecoder::name() const {
  if (!(part_seen_[0] && part_seen_[1] && part_seen_[2] && part_seen_[3])) {
    return {};
  }
  std::string out(name_, 8);
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  return out;
}

std::uint64_t RdsDecoder::name_packed() const {
  if (!(part_seen_[0] && part_seen_[1] && part_seen_[2] && part_seen_[3])) {
    return 0;
  }
  std::uint64_t packed = 0;
  for (unsigned i = 0; i < 8; ++i) {
    packed |= static_cast<std::uint64_t>(static_cast<unsigned char>(name_[i])) << (8 * i);
  }
  return packed;
}

} // namespace vocatron
