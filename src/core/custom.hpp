#pragma once

// Frequencies the person typed, so Scan list can walk those instead of a
// whole built-in band. Kept in channels/custom.toml (gitignored): it is
// personal, same as the saved-station file.

#include "channels.hpp"

#include <string>
#include <vector>

namespace vocatron {

std::string custom_path();

std::vector<Channel> load_custom(std::string *error);
bool write_custom(const std::vector<Channel> &stations, std::string *error);

// Turn text into stations. No radio, no file.
//
// One frequency:  "145.625"
// Several:        "145.500, 145.625 439.150"
// A grid:         "144-146/12.5"   (start-stop in MHz, step in kHz)
//
// `mode` is "am", "nfm", or "wfm" and applies to every frequency.
// At most 128 channels. `error` is empty when `channels` is usable.
struct CustomParse {
  std::vector<Channel> channels;
  std::string error;
};

CustomParse parse_custom_frequencies(const std::string &text, const std::string &mode);

// Add `channels` to the file. `replace` drops what was there. `added` is
// how many were not already in the list (same frequency and mode).
bool store_custom(const std::vector<Channel> &channels, bool replace, int *added,
                  std::string *error);

// Drop every custom station within 25 kHz of `hz`, any mode.
// `removed` is how many went. Returns false only when the file could not
// be written.
bool drop_custom_hz(std::uint64_t hz, int *removed, std::string *error);

} // namespace vocatron
