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

#include "bands.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vocatron {

struct ScanHit {
  std::uint64_t hz = 0;
  // "wfm", "am", or "nfm".
  std::string mode = "wfm";
  // FM scan: hump height over the band floor. Band search: for a talking
  // channel, its loudest moment over its usual level; for a steady one,
  // its usual level over the band floor. dB.
  float level_db = 0.0f;
  // FM scan only: 0 (buried) to 1 (clean), from listening to it.
  float clarity = 0.0f;
  std::string rds_name;
  std::uint16_t rds_pi = 0;
  // Band search only. Steady: on the whole time (a loop such as ATIS, a
  // beacon, or interference). Otherwise it came and went: someone talking.
  bool steady = false;
  // How many sweeps caught it above its usual level, and how long ago the
  // last one was, in seconds.
  int times_heard = 0;
  float last_heard_s = 0.0f;
  // Band search only: looks like interference, not a station. Either one
  // tooth of an evenly spaced comb of steady carriers (a switching power
  // supply or similar nearby), or one of several hits on whole megahertz
  // (the radio's own clock leaking in).
  bool interference = false;
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

// Watch a whole band for a while and list the channels that were in use.
//
// Short transmissions (a tower, a ship, a handheld) last a few seconds,
// so one look is not enough. The radio runs at 20 MS/s, one tuning covers
// about 7 MHz, and the band is swept over and over, about ten times a
// second for a band like airband. Every channel on the band's grid keeps a
// record of each visit. At the end each channel's usual level is the
// median of its visits, and the band's floor is the median of those.
struct BandSearchConfig {
  const Band *band = nullptr;
  std::uint32_t lna_db = 40;
  std::uint32_t vga_db = 26;
  bool amp = false;
  // How long to watch. Cancelling early still reports what was seen.
  double seconds = 45.0;
};

int run_band_search(const BandSearchConfig &config, std::vector<ScanHit> *hits,
                    ScanProgress progress, void *user);

// The analysis step on its own, so it can be tested without a radio.
// `visits[c]` holds channel c's power (dB) at each visit, in order, over
// `seconds` of watching. Channels may have different visit counts.
std::vector<ScanHit> find_activity(const Band &band, const std::vector<std::vector<float>> &visits,
                                   double seconds);

} // namespace vocatron
