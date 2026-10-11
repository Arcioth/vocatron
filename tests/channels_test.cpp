#include "core/channels.hpp"

#include <doctest.h>

#include <string>

namespace {

const vocatron::Channel *find(const vocatron::ChannelBook &book, const char *name) {
  for (const vocatron::Channel &channel : book.channels) {
    if (channel.name == name) {
      return &channel;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("channel file keeps the named frequencies") {
  const vocatron::ChannelBook &book = vocatron::channel_book();
  REQUIRE(book.error.empty());
  CHECK(book.channels.size() == 57);

  const vocatron::Channel *power = find(book, "POWER_FM");
  REQUIRE(power != nullptr);
  CHECK(power->hz == 100'000'000);
  CHECK(power->mode == "wfm");
  CHECK(power->group == "broadcast");

  const vocatron::Channel *atis = find(book, "LTBA_ATIS");
  REQUIRE(atis != nullptr);
  CHECK(atis->hz == 128'200'000);
  CHECK(atis->mode == "am");
  CHECK(atis->group == "airband");

  const vocatron::Channel *ch16 = find(book, "CH16");
  REQUIRE(ch16 != nullptr);
  CHECK(ch16->hz == 156'800'000);
  CHECK(ch16->mode == "nfm");
  CHECK(ch16->group == "marine");
  CHECK(ch16->bw_khz == doctest::Approx(25.0));

  const vocatron::Channel *pmr = find(book, "PMR1");
  REQUIRE(pmr != nullptr);
  CHECK(pmr->hz == 446'006'250);
  CHECK(pmr->mode == "nfm");
  CHECK(pmr->group == "pmr");

  int air = 0;
  int marine = 0;
  for (const vocatron::Channel &channel : book.channels) {
    if (channel.group == "airband") {
      ++air;
    }
    if (channel.group == "marine") {
      ++marine;
    }
    CHECK_FALSE(channel.description.empty());
  }
  CHECK(air == 17);
  CHECK(marine == 6);
}
