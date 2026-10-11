#pragma once

// Bands vocatron can search, with the channel grid and the mode used there.
// Receive only. These are listening bands; nothing here transmits.

#include <cstddef>
#include <cstdint>

namespace vocatron {

struct Band {
  const char *key;
  const char *label;
  // A few letters, for the band picker and for naming found channels.
  const char *short_label;
  // First and last channel centre.
  std::uint64_t first_hz;
  std::uint64_t last_hz;
  // Channel spacing. Airband uses 8.33 kHz steps (25 kHz / 3).
  double step_hz;
  // "am", "nfm", or "wfm".
  const char *mode;
  double bw_khz;
  // Power is measured over ± this around each channel centre.
  double half_width_hz;
};

// "fm" is broadcast FM, searched by the FM scan (it also decodes RDS).
inline constexpr Band kBands[] = {
    {"fm", "FM radio", "FM", 87'600'000, 107'900'000, 100'000.0, "wfm", 200, 75'000.0},
    {"airband", "Airband", "AIR", 118'000'000, 136'975'000, 25'000.0 / 3.0, "am", 8.33, 2'500.0},
    {"marine", "Marine", "MAR", 156'000'000, 162'025'000, 25'000.0, "nfm", 25, 6'000.0},
    {"2m", "2 m ham", "2M", 144'000'000, 146'000'000, 12'500.0, "nfm", 12.5, 4'000.0},
    {"70cm", "70 cm ham", "70CM", 430'000'000, 440'000'000, 12'500.0, "nfm", 12.5, 4'000.0},
    {"pmr", "PMR446", "PMR", 446'006'250, 446'193'750, 12'500.0, "nfm", 12.5, 4'000.0},
    {"cb", "CB 27 MHz", "CB", 26'965'000, 27'405'000, 10'000.0, "am", 10, 3'500.0},
    {"sw49", "SW 49 m", "SW49", 5'900'000, 6'200'000, 5'000.0, "am", 5, 2'500.0},
    {"sw41", "SW 41 m", "SW41", 7'200'000, 7'450'000, 5'000.0, "am", 5, 2'500.0},
    {"sw31", "SW 31 m", "SW31", 9'400'000, 9'900'000, 5'000.0, "am", 5, 2'500.0},
    {"sw25", "SW 25 m", "SW25", 11'600'000, 12'100'000, 5'000.0, "am", 5, 2'500.0},
};

inline constexpr std::size_t kBandCount = sizeof(kBands) / sizeof(kBands[0]);

// Number of channels on a band's grid.
inline std::size_t band_channels(const Band &band) {
  return static_cast<std::size_t>(
             (static_cast<double>(band.last_hz - band.first_hz) / band.step_hz) + 0.5) +
         1;
}

// Centre of channel `index`, rounded to the hertz.
inline std::uint64_t band_channel_hz(const Band &band, std::size_t index) {
  return band.first_hz +
         static_cast<std::uint64_t>(static_cast<double>(index) * band.step_hz + 0.5);
}

} // namespace vocatron
