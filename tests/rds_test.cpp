// RDS: build a station that sends the name "VOCATRON" and read it back.

#include "core/dsp.hpp"
#include "core/rds.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
constexpr std::uint16_t kPi = 0x7A12;

// The RDS bit stream: group 0A, name parts 0..3, over and over.
std::vector<unsigned> rds_bits(const std::string &name, std::size_t groups) {
  std::vector<unsigned> bits;
  for (std::size_t g = 0; g < groups; ++g) {
    const unsigned part = static_cast<unsigned>(g % 4);
    const std::uint16_t data[4] = {
        kPi,
        static_cast<std::uint16_t>(part), // group 0A, segment `part`
        0xE0CD,
        static_cast<std::uint16_t>((static_cast<unsigned char>(name[part * 2]) << 8) |
                                   static_cast<unsigned char>(name[part * 2 + 1])),
    };
    for (unsigned b = 0; b < 4; ++b) {
      const std::uint32_t block = vocatron::rds_make_block(data[b], b);
      for (int i = 25; i >= 0; --i) {
        bits.push_back((block >> i) & 1u);
      }
    }
  }
  return bits;
}

// The demodulated broadcast at time t: a tone, the 19 kHz pilot, and RDS
// on 57 kHz. Units are the demodulator's (deviation / 240 kHz).
struct Mpx {
  std::vector<int> symbols; // ±1, two per bit
  double operator()(double t) const {
    const double sym_pos = t * 2375.0;
    const auto index = static_cast<std::size_t>(sym_pos);
    double rds = 0.0;
    if (index < symbols.size()) {
      const double frac = sym_pos - static_cast<double>(index);
      rds = symbols[index] * std::sin(3.14159265358979323846 * frac);
    }
    return (40e3 * std::sin(kTwoPi * 1000.0 * t) + 7.5e3 * std::sin(kTwoPi * 19000.0 * t) +
            3e3 * rds * std::cos(kTwoPi * 57000.0 * t + 1.0)) /
           240e3;
  }
};

Mpx make_mpx(const std::string &name, std::size_t groups) {
  Mpx mpx;
  unsigned level = 0;
  for (unsigned bit : rds_bits(name, groups)) {
    level ^= bit; // send "changed" for 1
    mpx.symbols.push_back(level != 0 ? 1 : -1);
    mpx.symbols.push_back(level != 0 ? -1 : 1);
  }
  return mpx;
}

std::uint8_t clamp_byte(double v) {
  const long x = std::clamp(std::lround(v), -127L, 127L);
  return static_cast<std::uint8_t>(static_cast<std::int8_t>(x));
}

} // namespace

TEST_CASE("RDS checksums mark each block") {
  for (unsigned index = 0; index < 5; ++index) {
    for (std::uint16_t data : {0x0000, 0x1234, 0xFFFF, 0x7A12}) {
      CHECK(vocatron::rds_syndrome(vocatron::rds_make_block(data, index)) ==
            vocatron::rds_offset(index));
    }
  }
  // One flipped bit is caught.
  const std::uint32_t block = vocatron::rds_make_block(0x1234, 1);
  CHECK(vocatron::rds_syndrome(block ^ (1u << 13)) != vocatron::rds_offset(1));
}

TEST_CASE("RDS decoder reads the station name from the demodulated signal") {
  const Mpx mpx = make_mpx("VOCATRON", 40);
  vocatron::RdsDecoder rds;
  REQUIRE(rds.ok());
  constexpr double kRate = 240e3;
  std::vector<float> block(2400);
  for (std::size_t start = 0; start < static_cast<std::size_t>(3.0 * kRate);
       start += block.size()) {
    for (std::size_t i = 0; i < block.size(); ++i) {
      block[i] = static_cast<float>(mpx(static_cast<double>(start + i) / kRate));
    }
    rds.process(block.data(), block.size());
  }
  CHECK(rds.name() == "VOCATRON");
  CHECK(rds.pi() == kPi);
  CHECK(rds.good_blocks() > 50);
}

TEST_CASE("wide FM finds the RDS name in a noisy station") {
  const Mpx mpx = make_mpx("VOCATRON", 40);
  constexpr std::uint32_t kRate = 2'400'000;
  std::mt19937 rng(3);
  std::normal_distribution<double> gauss(0.0, 1.0);
  constexpr double kAmp = 0.3;
  // 25 dB signal-to-noise in 200 kHz: clean to the ear, a fair test for RDS.
  const double sigma = std::sqrt(kAmp * kAmp / std::pow(10.0, 2.5) * (kRate / 200e3) / 2.0) * 127.0;
  std::vector<std::uint8_t> iq(static_cast<std::size_t>(kRate) * 2 * 3); // 3 s
  double phase = 0.0;
  for (std::size_t n = 0; n < iq.size() / 2; ++n) {
    const double t = static_cast<double>(n) / kRate;
    phase += kTwoPi * (250e3 + mpx(t) * 240e3) / kRate;
    iq[2 * n] = clamp_byte(kAmp * 127.0 * std::cos(phase) + sigma * gauss(rng));
    iq[2 * n + 1] = clamp_byte(kAmp * 127.0 * std::sin(phase) + sigma * gauss(rng));
  }
  vocatron::WfmStream stream(kRate, 250'000.0f);
  std::vector<float> audio(8192);
  for (std::size_t off = 0; off < iq.size(); off += 1u << 18) {
    const std::size_t n = std::min<std::size_t>(1u << 18, iq.size() - off);
    stream.process(iq.data() + off, n, audio.data(), audio.size());
  }
  CHECK(stream.rds_name() == "VOCATRON");
  CHECK(stream.rds_pi() == kPi);
}
