#pragma once

// Restricted frequencies: public-safety radio such as the coast guard.
//
// Under Turkish law (TCK 132 and 133) listening to public-safety radio
// (police, gendarmerie, coast guard, fire, ambulance) without permission,
// or storing or sharing what was heard, violates the privacy of
// communications. vocatron keeps a list of such ranges in
// channels/restricted.toml. It will not tune into one until the person
// confirms they hold the licence and permission and take all
// responsibility, and it never records one.

#include <cstdint>
#include <string>
#include <vector>

namespace vocatron {

struct Restriction {
  std::string name;
  std::uint64_t from_hz = 0;
  std::uint64_t to_hz = 0;
  std::string reason;
};

// Loaded once from channels/restricted.toml (next to istanbul.toml).
// A missing file means no restrictions. `error` is empty when it parsed.
struct RestrictionBook {
  std::vector<Restriction> ranges;
  std::string path;
  std::string error;
};

const RestrictionBook &restriction_book();

// Same parsing, for a file of your choosing (tests use a temporary one).
RestrictionBook load_restrictions(const std::string &path);

// The range `hz` falls in, or null. `book` defaults to restriction_book().
const Restriction *find_restriction(std::uint64_t hz, const RestrictionBook *book = nullptr);

// Whether a listen may go ahead. Returns false and fills `why` when not:
// a restricted frequency without confirmed permission, or any attempt to
// record a restricted frequency.
bool listen_allowed(std::uint64_t hz, bool permission_confirmed, bool recording, std::string *why,
                    const RestrictionBook *book = nullptr);

// The words shown before tuning into a restricted range, in Turkish and English.
std::string restriction_warning(const Restriction &restriction);

// What the person types at the command line to accept.
inline constexpr const char *kAcceptPhrase = "KABUL EDIYORUM";

} // namespace vocatron
