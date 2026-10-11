// Custom scan frequencies. No radio.

#include "core/custom.hpp"
#include "core/saved.hpp"

#include <doctest.h>

#include <filesystem>
#include <fstream>

TEST_CASE("one frequency, a list, and a range") {
  const vocatron::CustomParse one = vocatron::parse_custom_frequencies("145.625", "nfm");
  CHECK(one.error.empty());
  REQUIRE(one.channels.size() == 1);
  CHECK(one.channels[0].hz == 145'625'000);
  CHECK(one.channels[0].mode == "nfm");
  CHECK(one.channels[0].group == "custom");
  CHECK(one.channels[0].name == "145.625");

  const vocatron::CustomParse list =
      vocatron::parse_custom_frequencies("145.500, 145.625 439.150", "nfm");
  CHECK(list.error.empty());
  REQUIRE(list.channels.size() == 3);
  CHECK(list.channels[2].hz == 439'150'000);

  const vocatron::CustomParse range = vocatron::parse_custom_frequencies("144-144.05/12.5", "nfm");
  CHECK(range.error.empty());
  REQUIRE(range.channels.size() == 5);
  CHECK(range.channels[0].hz == 144'000'000);
  CHECK(range.channels[4].hz == 144'050'000);

  CHECK_FALSE(vocatron::parse_custom_frequencies("", "nfm").error.empty());
  CHECK_FALSE(vocatron::parse_custom_frequencies("145.625", "usb").error.empty());
  CHECK_FALSE(vocatron::parse_custom_frequencies("0.1", "am").error.empty());
  CHECK_FALSE(vocatron::parse_custom_frequencies("1-6000/1", "nfm").error.empty());
}

TEST_CASE("custom frequencies round-trip through the file") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "vocatron_custom_test";
  fs::create_directories(dir);
  const fs::path book = dir / "istanbul.toml";
  {
    std::ofstream out(book);
    out << "[[channel]]\nname = \"POWER_FM\"\nfreq_mhz = 100.0\nmode = \"wfm\"\nbw_khz = 200\n"
           "group = \"broadcast\"\ndescription = \"test\"\n";
  }
  // custom_path() sits next to the channel book. Point the book at this file
  // by... the book is loaded once from the real tree. Write through the
  // saved-file helpers instead, which is what store uses under the hood.
  const std::string path = (dir / "custom.toml").string();
  const vocatron::CustomParse parsed = vocatron::parse_custom_frequencies("145.625,145.650", "nfm");
  REQUIRE(parsed.error.empty());
  std::string error;
  REQUIRE(vocatron::write_saved_file(path, parsed.channels, &error, "custom"));
  const std::vector<vocatron::Channel> back = vocatron::load_saved_file(path, &error, "custom");
  CHECK(error.empty());
  REQUIRE(back.size() == 2);
  CHECK(back[0].group == "custom");
  CHECK(back[0].hz == 145'625'000);
  CHECK(back[1].name == "145.65");
  fs::remove_all(dir);
}
