#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vocatron {

// One named station from channels/istanbul.toml.
struct Channel {
  std::string name;
  std::uint64_t hz = 0;
  std::string mode;
  double bw_khz = 0;
  std::string group;
  std::string description;
};

// Loaded once. `error` is empty when the file parsed.
// Pointers into `channels` stay valid for the rest of the process.
struct ChannelBook {
  std::vector<Channel> channels;
  std::string path;
  std::string error;
};

const ChannelBook &channel_book();

// Print the book. `group` nullptr prints every channel.
// Returns 0 on success.
int print_channel_list(const char *group);

} // namespace vocatron
