#include "core/saved.hpp"

#include <doctest.h>

#include <filesystem>
#include <string>
#include <vector>

TEST_CASE("saved stations survive a write and a read") {
  namespace fs = std::filesystem;
  const fs::path path = fs::temp_directory_path() / "vocatron_saved_test.toml";
  std::vector<vocatron::Channel> stations(2);
  stations[0].name = "FENOMEN";
  stations[0].hz = 100'400'000;
  stations[0].mode = "wfm";
  stations[0].bw_khz = 200;
  stations[0].description = "Found by scan, RDS \"FENOMEN\" \\ PI 3474";
  stations[1].name = "LTFM_ATIS";
  stations[1].hz = 128'200'000;
  stations[1].mode = "am";
  stations[1].bw_khz = 8.33;
  std::string error;
  REQUIRE(vocatron::write_saved_file(path.string(), stations, &error));
  const std::vector<vocatron::Channel> back = vocatron::load_saved_file(path.string(), &error);
  CHECK(error.empty());
  REQUIRE(back.size() == 2);
  CHECK(back[0].name == "FENOMEN");
  CHECK(back[0].hz == 100'400'000);
  CHECK(back[0].mode == "wfm");
  CHECK(back[0].group == "saved");
  CHECK(back[0].description == stations[0].description);
  CHECK(back[1].hz == 128'200'000);
  CHECK(vocatron::same_station(back[0], stations[0]));
  CHECK_FALSE(vocatron::same_station(back[0], back[1]));
  fs::remove(path);
}

TEST_CASE("no saved file means no saved stations") {
  std::string error;
  CHECK(vocatron::load_saved_file("/nonexistent/vocatron/saved.toml", &error).empty());
  CHECK(error.empty());
}
