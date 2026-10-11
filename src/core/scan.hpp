#pragma once

// Find FM broadcast stations that this radio can actually hear.
//
// 1. Survey: step the tuner across the band, take an averaged spectrum at
//    each step, and measure every channel on the 100 kHz grid. A station is
//    a hump about 200 kHz wide that stands above the band's floor.
// 2. Verify: tune each hump, demodulate it, and measure how clean it is
//    (clarity) and its RDS name. Pure noise is dropped.
//
// Receive only. Opens the HackRF itself, so nothing else may hold it.

#include <cstdint>
#include <string>
#include <vector>

namespace vocatron {

struct ScanHit {
  std::uint64_t hz = 0;
  // Hump height over the band floor, dB.
  float level_db = 0.0f;
  // 0 (buried) to 1 (clean), from listening to it.
  float clarity = 0.0f;
  std::string rds_name;
  std::uint16_t rds_pi = 0;
};

struct ScanConfig {
  std::uint64_t start_hz = 87'500'000;
  std::uint64_t stop_hz = 108'000'000;
  std::uint32_t lna_db = 40;
  std::uint32_t vga_db = 26;
  bool amp = false;
  // A hump must stand this far over the floor to be listened to.
  float min_level_db = 4.0f;
  // Below this clarity after listening, it is not kept.
  float min_clarity = 0.15f;
  // Seconds recorded at each tuner position. Every candidate in that
  // window is decoded from it, so this is also how long RDS gets.
  double max_listen_s = 2.5;
};

// `fraction` 0..1 and a short line of what is happening.
// Return false to cancel the scan.
using ScanProgress = bool (*)(float fraction, const char *what, void *user);

// 0: finished (or cancelled; `hits` has what was found so far).
// 1: the radio could not be opened or stopped streaming.
int run_fm_scan(const ScanConfig &config, std::vector<ScanHit> *hits, ScanProgress progress,
                void *user);

// A name for a found station: its RDS name, or "FM 100.0".
std::string scan_hit_name(const ScanHit &hit);

} // namespace vocatron
