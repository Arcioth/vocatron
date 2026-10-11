// Named stations live in channels/istanbul.toml so a new one is a few
// lines in that file, not a rebuild of the radio code.

#include "channels.hpp"

#include <toml.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace vocatron {
namespace {

bool known_mode(const std::string &mode) { return mode == "wfm" || mode == "am" || mode == "nfm"; }

std::optional<std::filesystem::path> find_book_path() {
  namespace fs = std::filesystem;
  const fs::path relative = fs::path("channels") / "istanbul.toml";
  std::error_code ec;
  if (fs::is_regular_file(relative, ec)) {
    return fs::absolute(relative);
  }
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return std::nullopt;
  }
  fs::path dir = exe.parent_path();
  for (int hop = 0; hop < 6; ++hop) {
    const fs::path candidate = dir / relative;
    if (fs::is_regular_file(candidate, ec)) {
      return candidate;
    }
    if (dir.empty() || dir == dir.parent_path()) {
      break;
    }
    dir = dir.parent_path();
  }
  return std::nullopt;
}

std::string need_string(const toml::table &row, const char *key, std::string *error) {
  const auto value = row[key].value<std::string>();
  if (!value || value->empty()) {
    *error = std::string("missing ") + key;
    return {};
  }
  return *value;
}

ChannelBook load_book() {
  ChannelBook book;
  const std::optional<std::filesystem::path> path = find_book_path();
  if (!path) {
    book.error = "could not find channels/istanbul.toml\n"
                 "Run vocatron from the vocatron directory, or keep that file next to the program.";
    return book;
  }
  book.path = path->string();

  toml::table table;
  try {
    table = toml::parse_file(book.path);
  } catch (const toml::parse_error &err) {
    book.error = book.path + ": " + std::string(err.description());
    return book;
  }

  const toml::array *rows = table.get_as<toml::array>("channel");
  if (rows == nullptr || rows->empty()) {
    book.error = book.path + ": needs one or more [[channel]] entries";
    return book;
  }

  for (std::size_t i = 0; i < rows->size(); ++i) {
    const toml::table *row = rows->at(i).as_table();
    const std::string where = book.path + " channel " + std::to_string(i + 1);
    if (row == nullptr) {
      book.error = where + " is not a table";
      book.channels.clear();
      return book;
    }
    std::string field_error;
    Channel channel;
    channel.name = need_string(*row, "name", &field_error);
    channel.mode = need_string(*row, "mode", &field_error);
    channel.group = need_string(*row, "group", &field_error);
    channel.description = need_string(*row, "description", &field_error);
    const auto mhz = row->get("freq_mhz") == nullptr ? std::optional<double>{}
                                                     : (*row)["freq_mhz"].value<double>();
    const auto bw =
        row->get("bw_khz") == nullptr ? std::optional<double>{} : (*row)["bw_khz"].value<double>();
    if (!field_error.empty() || !mhz || !bw) {
      book.error =
          where + (field_error.empty() ? " is missing freq_mhz or bw_khz" : ": " + field_error);
      book.channels.clear();
      return book;
    }
    if (*mhz < 1.0 || *mhz > 6000.0) {
      book.error = where + " frequency is out of range";
      book.channels.clear();
      return book;
    }
    if (*bw <= 0.0 || *bw > 500.0) {
      book.error = where + " bw_khz is out of range";
      book.channels.clear();
      return book;
    }
    if (!known_mode(channel.mode)) {
      book.error = where + " mode must be wfm, am, or nfm";
      book.channels.clear();
      return book;
    }
    channel.hz = static_cast<std::uint64_t>(std::llround(*mhz * 1'000'000.0));
    channel.bw_khz = *bw;
    for (const Channel &earlier : book.channels) {
      if (earlier.name == channel.name) {
        book.error = where + " repeats the name " + channel.name;
        book.channels.clear();
        return book;
      }
    }
    book.channels.push_back(std::move(channel));
  }
  return book;
}

} // namespace

const ChannelBook &channel_book() {
  static const ChannelBook book = load_book();
  return book;
}

int print_channel_list(const char *group) {
  const ChannelBook &book = channel_book();
  if (!book.error.empty()) {
    std::fprintf(stderr, "%s\n", book.error.c_str());
    return 1;
  }

  std::vector<const Channel *> shown;
  shown.reserve(book.channels.size());
  for (const Channel &channel : book.channels) {
    if (group == nullptr || channel.group == group) {
      shown.push_back(&channel);
    }
  }
  if (shown.empty()) {
    std::fprintf(stderr, "no channels in group \"%s\"\n", group != nullptr ? group : "");
    std::fprintf(stderr, "groups:");
    std::string previous;
    for (const Channel &channel : book.channels) {
      if (channel.group == previous) {
        continue;
      }
      std::fprintf(stderr, " %s", channel.group.c_str());
      previous = channel.group;
    }
    std::fprintf(stderr, "\n");
    return 1;
  }

  std::printf("%-16s %12s  %-4s %7s  %-10s  %s\n", "name", "MHz", "mode", "kHz", "group", "what");
  for (const Channel *channel : shown) {
    std::printf("%-16s %12.6f  %-4s %7.2f  %-10s  %s\n", channel->name.c_str(),
                static_cast<double>(channel->hz) / 1.0e6, channel->mode.c_str(), channel->bw_khz,
                channel->group.c_str(), channel->description.c_str());
  }
  std::printf("%zu channel%s", shown.size(), shown.size() == 1 ? "" : "s");
  if (group != nullptr) {
    std::printf(" in %s", group);
  }
  std::printf("\n");
  return 0;
}

} // namespace vocatron
