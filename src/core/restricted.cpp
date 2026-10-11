#include "restricted.hpp"

#include "channels.hpp"

#include <toml.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace vocatron {
namespace {

std::string restricted_path() {
  namespace fs = std::filesystem;
  const ChannelBook &book = channel_book();
  if (!book.path.empty()) {
    return (fs::path(book.path).parent_path() / "restricted.toml").string();
  }
  return (fs::path("channels") / "restricted.toml").string();
}

std::uint64_t mhz_to_hz(double mhz) {
  return static_cast<std::uint64_t>(std::llround(mhz * 1'000'000.0));
}

} // namespace

RestrictionBook load_restrictions(const std::string &path) {
  RestrictionBook book;
  book.path = path;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return book;
  }
  toml::table table;
  try {
    table = toml::parse_file(path);
  } catch (const toml::parse_error &err) {
    book.error = path + ": " + std::string(err.description());
    return book;
  }
  const toml::array *rows = table.get_as<toml::array>("restricted");
  if (rows == nullptr) {
    return book;
  }
  for (std::size_t i = 0; i < rows->size(); ++i) {
    const toml::table *row = rows->at(i).as_table();
    if (row == nullptr) {
      continue;
    }
    Restriction r;
    r.name = (*row)["name"].value_or(std::string());
    r.reason = (*row)["reason"].value_or(std::string());
    const double from = (*row)["from_mhz"].value_or(0.0);
    const double to = (*row)["to_mhz"].value_or(0.0);
    if (r.name.empty() || from < 1.0 || to < from) {
      book.error = path + " entry " + std::to_string(i + 1) +
                   ": needs name, from_mhz and to_mhz (to_mhz not below from_mhz)";
      continue;
    }
    r.from_hz = mhz_to_hz(from);
    r.to_hz = mhz_to_hz(to);
    book.ranges.push_back(r);
  }
  return book;
}

const RestrictionBook &restriction_book() {
  static const RestrictionBook book = load_restrictions(restricted_path());
  return book;
}

const Restriction *find_restriction(std::uint64_t hz, const RestrictionBook *book) {
  const RestrictionBook &b = book != nullptr ? *book : restriction_book();
  for (const Restriction &r : b.ranges) {
    if (hz >= r.from_hz && hz <= r.to_hz) {
      return &r;
    }
  }
  return nullptr;
}

bool listen_allowed(std::uint64_t hz, bool permission_confirmed, bool recording, std::string *why,
                    const RestrictionBook *book) {
  const Restriction *r = find_restriction(hz, book);
  if (r == nullptr) {
    return true;
  }
  if (recording) {
    if (why != nullptr) {
      *why = r->name +
             " is restricted. vocatron never records it: storing what was heard is itself an "
             "offence (TCK 132, 133).";
    }
    return false;
  }
  if (!permission_confirmed) {
    if (why != nullptr) {
      *why = r->name +
             " is restricted. Confirm that you hold the licence and permission and take all "
             "responsibility before listening.";
    }
    return false;
  }
  return true;
}

std::string restriction_warning(const Restriction &r) {
  char range[64];
  std::snprintf(range, sizeof range, "%.4f to %.4f MHz", static_cast<double>(r.from_hz) / 1e6,
                static_cast<double>(r.to_hz) / 1e6);
  std::string text = "RESTRICTED: " + r.name + " (" + range + ")";
  if (!r.reason.empty()) {
    text += ", " + r.reason;
  }
  text += ".\n\n"
          "Bu frekans kamu güvenliği haberleşmesidir. İzinsiz dinlemek, kaydetmek veya "
          "paylaşmak TCK 132 ve 133 kapsamında haberleşmenin gizliliğini ihlal suçudur.\n"
          "This is public-safety radio. Listening without permission, or recording or sharing "
          "it, is a crime under the Turkish Penal Code (articles 132 and 133).\n\n"
          "Continue only if you hold the licence and permission to listen to it. By continuing "
          "you take all responsibility. vocatron will not record it.";
  return text;
}

} // namespace vocatron
