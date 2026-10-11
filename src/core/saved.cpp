#include "saved.hpp"

#include <toml.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace vocatron {
namespace {

// TOML basic string: quote and backslash escaped, control characters dropped.
std::string quoted(const std::string &text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      out += c;
    }
  }
  out += '"';
  return out;
}

} // namespace

std::string saved_path() {
  namespace fs = std::filesystem;
  const ChannelBook &book = channel_book();
  if (!book.path.empty()) {
    return (fs::path(book.path).parent_path() / "saved.toml").string();
  }
  return (fs::path("channels") / "saved.toml").string();
}

std::vector<Channel> load_saved(std::string *error) { return load_saved_file(saved_path(), error); }

bool write_saved(const std::vector<Channel> &stations, std::string *error) {
  return write_saved_file(saved_path(), stations, error);
}

std::vector<Channel> load_saved_file(const std::string &path, std::string *error,
                                     const char *group) {
  std::vector<Channel> out;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return out;
  }
  toml::table table;
  try {
    table = toml::parse_file(path);
  } catch (const toml::parse_error &err) {
    if (error != nullptr) {
      *error = path + ": " + std::string(err.description());
    }
    return out;
  }
  const toml::array *rows = table.get_as<toml::array>("channel");
  if (rows == nullptr) {
    return out;
  }
  for (const toml::node &node : *rows) {
    const toml::table *row = node.as_table();
    if (row == nullptr) {
      continue;
    }
    Channel channel;
    channel.name = (*row)["name"].value_or(std::string());
    const double mhz = (*row)["freq_mhz"].value_or(0.0);
    channel.mode = (*row)["mode"].value_or(std::string());
    channel.bw_khz = (*row)["bw_khz"].value_or(0.0);
    channel.group = group != nullptr ? group : "saved";
    channel.description = (*row)["description"].value_or(std::string());
    if (channel.name.empty() || mhz < 1.0 ||
        (channel.mode != "wfm" && channel.mode != "am" && channel.mode != "nfm")) {
      continue;
    }
    channel.hz = static_cast<std::uint64_t>(std::llround(mhz * 1'000'000.0));
    out.push_back(channel);
  }
  return out;
}

bool write_saved_file(const std::string &path, const std::vector<Channel> &stations,
                      std::string *error, const char *group) {
  const std::string temp = path + ".new";
  {
    std::ofstream file(temp, std::ios::trunc);
    if (!file) {
      if (error != nullptr) {
        *error = "could not write " + temp;
      }
      return false;
    }
    if (group != nullptr && std::strcmp(group, "custom") == 0) {
      file << "# Frequencies you asked vocatron to scan.\n"
              "# Same format as istanbul.toml. Safe to edit by hand.\n";
    } else {
      file << "# Stations you listened to and saved in vocatron tui.\n"
              "# Same format as istanbul.toml. Safe to edit by hand.\n";
    }
    for (const Channel &channel : stations) {
      char mhz[32];
      std::snprintf(mhz, sizeof mhz, "%.4f", static_cast<double>(channel.hz) / 1.0e6);
      file << "\n[[channel]]\n"
           << "name = " << quoted(channel.name) << "\n"
           << "freq_mhz = " << mhz << "\n"
           << "mode = " << quoted(channel.mode) << "\n"
           << "bw_khz = " << channel.bw_khz << "\n"
           << "group = " << quoted(group != nullptr ? group : "saved") << "\n"
           << "description = " << quoted(channel.description) << "\n";
    }
    if (!file) {
      if (error != nullptr) {
        *error = "could not write " + temp;
      }
      return false;
    }
  }
  // Replace in one step, so a crash never leaves half a file.
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "could not replace " + path + ": " + ec.message();
    }
    return false;
  }
  return true;
}

bool same_station(const Channel &a, const Channel &b) {
  const auto gap = a.hz > b.hz ? a.hz - b.hz : b.hz - a.hz;
  return a.mode == b.mode && gap <= 25'000;
}

} // namespace vocatron
