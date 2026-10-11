#include "custom.hpp"

#include "saved.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

namespace vocatron {
namespace {

constexpr int kMaxCustom = 128;

std::string trim(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string mhz_name(std::uint64_t hz) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.5f", static_cast<double>(hz) / 1.0e6);
  std::string name = buf;
  while (name.size() > 1 && name.back() == '0') {
    name.pop_back();
  }
  if (!name.empty() && name.back() == '.') {
    name.pop_back();
  }
  return name;
}

double bandwidth_khz(const std::string &mode) {
  if (mode == "wfm") {
    return 200.0;
  }
  if (mode == "am") {
    return 8.33;
  }
  return 12.5;
}

bool parse_double(const std::string &text, double *out) {
  if (text.empty()) {
    return false;
  }
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

Channel make_channel(std::uint64_t hz, const std::string &mode) {
  Channel channel;
  channel.hz = hz;
  channel.mode = mode;
  channel.bw_khz = bandwidth_khz(mode);
  channel.group = "custom";
  channel.name = mhz_name(hz);
  channel.description = "Custom scan frequency";
  return channel;
}

bool in_range(std::uint64_t hz) { return hz >= 1'000'000 && hz <= 6'000'000'000ULL; }

// One MHz number, or a start-stop/step grid. Appends to `out`.
bool take_piece(const std::string &piece, const std::string &mode, std::vector<Channel> *out,
                std::string *error) {
  const std::size_t slash = piece.find('/');
  if (slash == std::string::npos) {
    double mhz = 0.0;
    if (!parse_double(piece, &mhz)) {
      *error = "\"" + piece + "\" is not a frequency in MHz.";
      return false;
    }
    const auto hz = static_cast<std::uint64_t>(std::llround(mhz * 1'000'000.0));
    if (!in_range(hz)) {
      *error = "Frequencies stay between 1 and 6000 MHz.";
      return false;
    }
    out->push_back(make_channel(hz, mode));
    return true;
  }

  const std::string range = piece.substr(0, slash);
  const std::string step_text = piece.substr(slash + 1);
  const std::size_t dash = range.find('-');
  if (dash == std::string::npos || dash == 0 || dash + 1 >= range.size()) {
    *error = "A range looks like 144-146/12.5 (start-stop in MHz, step in kHz).";
    return false;
  }
  double start_mhz = 0.0;
  double stop_mhz = 0.0;
  double step_khz = 0.0;
  if (!parse_double(range.substr(0, dash), &start_mhz) ||
      !parse_double(range.substr(dash + 1), &stop_mhz) || !parse_double(step_text, &step_khz)) {
    *error = "A range looks like 144-146/12.5 (start-stop in MHz, step in kHz).";
    return false;
  }
  if (stop_mhz < start_mhz || step_khz <= 0.0) {
    *error = "The end of a range has to be above the start, and the step above 0.";
    return false;
  }
  const auto start_hz = static_cast<std::uint64_t>(std::llround(start_mhz * 1'000'000.0));
  const auto stop_hz = static_cast<std::uint64_t>(std::llround(stop_mhz * 1'000'000.0));
  const auto step_hz = static_cast<std::uint64_t>(std::llround(step_khz * 1'000.0));
  if (step_hz == 0 || !in_range(start_hz) || !in_range(stop_hz)) {
    *error = "Frequencies stay between 1 and 6000 MHz, and the step has to be at least 1 Hz.";
    return false;
  }
  for (std::uint64_t hz = start_hz;; hz += step_hz) {
    if (out->size() >= static_cast<std::size_t>(kMaxCustom)) {
      *error = "That is more than 128 channels. Use a wider step, or a shorter range.";
      out->clear();
      return false;
    }
    out->push_back(make_channel(hz, mode));
    if (hz >= stop_hz || stop_hz - hz < step_hz) {
      break;
    }
  }
  return true;
}

} // namespace

std::string custom_path() {
  namespace fs = std::filesystem;
  const ChannelBook &book = channel_book();
  if (!book.path.empty()) {
    return (fs::path(book.path).parent_path() / "custom.toml").string();
  }
  return (fs::path("channels") / "custom.toml").string();
}

std::vector<Channel> load_custom(std::string *error) {
  return load_saved_file(custom_path(), error, "custom");
}

bool write_custom(const std::vector<Channel> &stations, std::string *error) {
  return write_saved_file(custom_path(), stations, error, "custom");
}

CustomParse parse_custom_frequencies(const std::string &text, const std::string &mode) {
  CustomParse parsed;
  if (mode != "am" && mode != "nfm" && mode != "wfm") {
    parsed.error = "Mode has to be am, nfm, or wfm.";
    return parsed;
  }
  const std::string body = trim(text);
  if (body.empty()) {
    parsed.error = "Type a frequency in MHz, such as 145.625, or a range such as 144-146/12.5.";
    return parsed;
  }
  std::string spaced = body;
  for (char &c : spaced) {
    if (c == ',') {
      c = ' ';
    }
  }
  std::istringstream in(spaced);
  std::string piece;
  while (in >> piece) {
    if (parsed.channels.size() >= static_cast<std::size_t>(kMaxCustom)) {
      parsed.error = "That is more than 128 channels. Use a wider step, or a shorter range.";
      parsed.channels.clear();
      return parsed;
    }
    if (!take_piece(piece, mode, &parsed.channels, &parsed.error)) {
      parsed.channels.clear();
      return parsed;
    }
  }
  if (parsed.channels.empty()) {
    parsed.error = "Type a frequency in MHz, such as 145.625, or a range such as 144-146/12.5.";
  }
  return parsed;
}

bool store_custom(const std::vector<Channel> &channels, bool replace, int *added,
                  std::string *error) {
  std::vector<Channel> list = replace ? std::vector<Channel>{} : load_custom(error);
  if (error != nullptr && !error->empty()) {
    return false;
  }
  int count = 0;
  for (const Channel &channel : channels) {
    const bool known = std::any_of(
        list.begin(), list.end(), [&](const Channel &have) { return same_station(have, channel); });
    if (!known) {
      list.push_back(channel);
      ++count;
    }
  }
  if (list.size() > static_cast<std::size_t>(kMaxCustom)) {
    if (error != nullptr) {
      *error = "The custom list holds at most 128 frequencies.";
    }
    return false;
  }
  if (!write_custom(list, error)) {
    return false;
  }
  if (added != nullptr) {
    *added = count;
  }
  return true;
}

bool drop_custom_hz(std::uint64_t hz, int *removed, std::string *error) {
  std::vector<Channel> list = load_custom(error);
  if (error != nullptr && !error->empty()) {
    return false;
  }
  const std::size_t before = list.size();
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const Channel &channel) {
                              const auto gap = channel.hz > hz ? channel.hz - hz : hz - channel.hz;
                              return gap <= 25'000;
                            }),
             list.end());
  if (!write_custom(list, error)) {
    return false;
  }
  if (removed != nullptr) {
    *removed = static_cast<int>(before - list.size());
  }
  return true;
}

} // namespace vocatron
