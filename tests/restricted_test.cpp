// Restricted-frequency book. No radio.

#include "core/channels.hpp"
#include "core/restricted.hpp"

#include <doctest.h>

#include <filesystem>
#include <fstream>

TEST_CASE("coast guard channel 67 is restricted and not in the station list") {
  const vocatron::ChannelBook &channels = vocatron::channel_book();
  REQUIRE(channels.error.empty());
  for (const vocatron::Channel &channel : channels.channels) {
    CHECK(channel.name != "CH67_SG");
    CHECK(channel.hz != 156'375'000);
  }

  const vocatron::RestrictionBook &book = vocatron::restriction_book();
  REQUIRE(book.error.empty());
  REQUIRE_FALSE(book.ranges.empty());

  const vocatron::Restriction *coast = vocatron::find_restriction(156'375'000);
  REQUIRE(coast != nullptr);
  CHECK(coast->name.find("Coast guard") != std::string::npos);
  // A 12.5 kHz channel either side of 156.375 stays inside the block.
  CHECK(vocatron::find_restriction(156'362'500) == coast);
  CHECK(vocatron::find_restriction(156'387'500) == coast);
  CHECK(vocatron::find_restriction(156'350'000) == nullptr);
  CHECK(vocatron::find_restriction(100'000'000) == nullptr);

  std::string why;
  CHECK(vocatron::listen_allowed(100'000'000, false, false, &why));
  CHECK_FALSE(vocatron::listen_allowed(156'375'000, false, false, &why));
  CHECK(why.find("restricted") != std::string::npos);
  CHECK_FALSE(vocatron::listen_allowed(156'375'000, true, true, &why));
  CHECK(why.find("never records") != std::string::npos);
  CHECK(vocatron::listen_allowed(156'375'000, true, false, &why));

  const std::string warning = vocatron::restriction_warning(*coast);
  CHECK(warning.find("TCK") != std::string::npos);
  CHECK(warning.find("KABUL") == std::string::npos);
}

TEST_CASE("a bad restricted file is reported and not applied") {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "vocatron_restricted_bad.toml";
  {
    std::ofstream out(path);
    out << "[[restricted]]\nname = \"\"\nfrom_mhz = 0\nto_mhz = 1\n";
  }
  const vocatron::RestrictionBook book = vocatron::load_restrictions(path.string());
  CHECK_FALSE(book.error.empty());
  CHECK(book.ranges.empty());
  CHECK(vocatron::find_restriction(1'000'000, &book) == nullptr);
  fs::remove(path);
}

TEST_CASE("a missing restricted file means nothing is restricted") {
  const vocatron::RestrictionBook book =
      vocatron::load_restrictions("/nonexistent/vocatron/restricted.toml");
  CHECK(book.error.empty());
  CHECK(book.ranges.empty());
  CHECK(vocatron::listen_allowed(156'375'000, false, true, nullptr, &book));
}
