#pragma once

// Stations the person listened to and confirmed they can hear.
//
// Kept in channels/saved.toml, next to istanbul.toml, in the same
// [[channel]] format, so a saved station can also be copied into the main
// file by hand. The file is personal, so git ignores it.

#include "channels.hpp"

#include <string>
#include <vector>

namespace vocatron {

// Where the saved stations live.
std::string saved_path();

// Empty when the file does not exist yet. `error` is set if it exists but
// could not be read; the stations that did parse are still returned.
std::vector<Channel> load_saved(std::string *error);

// Rewrites the whole file. Returns false and sets `error` on failure.
bool write_saved(const std::vector<Channel> &stations, std::string *error);

// The same, for a file of your choosing (tests use a temporary one).
std::vector<Channel> load_saved_file(const std::string &path, std::string *error);
bool write_saved_file(const std::string &path, const std::vector<Channel> &stations,
                      std::string *error);

// The same station: same frequency (within 25 kHz) and mode.
bool same_station(const Channel &a, const Channel &b);

} // namespace vocatron
