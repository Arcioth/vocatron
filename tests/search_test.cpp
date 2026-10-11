// Band search analysis on made-up sweeps: no radio.

#include "core/bands.hpp"
#include "core/scan.hpp"

#include <doctest.h>

#include <random>
#include <vector>

namespace {

const vocatron::Band &airband() {
  for (const vocatron::Band &band : vocatron::kBands) {
    if (std::string(band.key) == "airband") {
      return band;
    }
  }
  return vocatron::kBands[0];
}

} // namespace

TEST_CASE("band grid: airband steps 8.33 kHz from 118.000") {
  const vocatron::Band &band = airband();
  CHECK(vocatron::band_channel_hz(band, 0) == 118'000'000);
  CHECK(vocatron::band_channel_hz(band, 3) == 118'025'000);
  CHECK(vocatron::band_channel_hz(band, 1) == 118'008'333);
  CHECK(vocatron::band_channels(band) == 2278);
}

TEST_CASE("band search finds talk and steady carriers, not spikes or spill") {
  const vocatron::Band &band = airband();
  constexpr std::size_t kChannels = 60;
  constexpr std::size_t kSweeps = 100;
  std::mt19937 rng(5);
  std::normal_distribution<float> noise(-80.0f, 1.0f);
  std::vector<std::vector<float>> visits(kChannels);
  for (std::size_t c = 0; c < kChannels; ++c) {
    for (std::size_t s = 0; s < kSweeps; ++s) {
      float level = noise(rng);
      const bool talking = (s >= 30 && s < 40) || (s >= 70 && s < 76);
      if (c == 10 && talking) {
        level += 20.0f; // someone talking, twice
      }
      if ((c == 9 || c == 11) && talking) {
        level += 8.0f; // a little spills into the neighbours
      }
      if (c == 25) {
        level += 15.0f; // a carrier that never stops
      }
      if (c == 40 && s == 50) {
        level += 20.0f; // one noise spike
      }
      visits[c].push_back(level);
    }
  }
  const std::vector<vocatron::ScanHit> hits = vocatron::find_activity(band, visits, 10.0);
  REQUIRE(hits.size() == 2);
  CHECK(hits[0].hz == vocatron::band_channel_hz(band, 10));
  CHECK_FALSE(hits[0].steady);
  CHECK(hits[0].times_heard == 16);
  CHECK(hits[0].level_db > 15.0f);
  CHECK(hits[0].last_heard_s == doctest::Approx(2.4).epsilon(0.05));
  CHECK(hits[0].mode == "am");
  CHECK(hits[1].hz == vocatron::band_channel_hz(band, 25));
  CHECK(hits[1].steady);
}

TEST_CASE("band search flags an evenly spaced comb but keeps a lone carrier") {
  const vocatron::Band &band = airband();
  constexpr std::size_t kChannels = 700;
  std::mt19937 rng(9);
  std::normal_distribution<float> noise(-80.0f, 1.0f);
  std::vector<std::vector<float>> visits(kChannels);
  // 608.33 kHz is exactly 73 airband channels.
  const std::size_t comb[] = {100, 173, 246, 319, 392, 465, 538, 611};
  for (std::size_t c = 0; c < kChannels; ++c) {
    for (int s = 0; s < 50; ++s) {
      float level = noise(rng);
      for (const std::size_t tooth : comb) {
        if (c == tooth) {
          level += 12.0f;
        }
      }
      if (c == 290) {
        level += 15.0f; // an ATIS-like loop on its own
      }
      visits[c].push_back(level);
    }
  }
  const std::vector<vocatron::ScanHit> hits = vocatron::find_activity(band, visits, 5.0);
  int teeth = 0;
  bool lone_kept = false;
  for (const vocatron::ScanHit &hit : hits) {
    if (hit.hz == vocatron::band_channel_hz(band, 290)) {
      lone_kept = !hit.interference;
    } else if (hit.interference) {
      ++teeth;
    }
  }
  CHECK(teeth == 8);
  CHECK(lone_kept);
}
