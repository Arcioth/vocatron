#include "cli/tui_model.hpp"

#include <doctest.h>

TEST_CASE("gain knobs stay in the radio's range") {
  vocatron::Knobs knobs;
  CHECK(knobs.lna == 40);
  CHECK_FALSE(vocatron::step_lna(knobs, +1));
  CHECK(vocatron::step_lna(knobs, -1));
  CHECK(knobs.lna == 32);
  knobs.lna = 0;
  CHECK_FALSE(vocatron::step_lna(knobs, -1));

  CHECK(knobs.vga == 26);
  CHECK_FALSE(vocatron::step_vga(knobs, +1));
  CHECK(vocatron::step_vga(knobs, -1));
  CHECK(knobs.vga == 24);
  knobs.vga = 0;
  CHECK_FALSE(vocatron::step_vga(knobs, -1));

  // The amp starts off.
  CHECK_FALSE(knobs.amp);
}

TEST_CASE("squelch moves in 2 dB steps and is kept until the station changes") {
  vocatron::Knobs knobs;
  vocatron::enter_station(knobs, "nfm", false);
  CHECK(knobs.squelch_on);
  CHECK(knobs.squelch_db == doctest::Approx(-48.0f));
  // Broadcast FM starts with the gate off.
  vocatron::enter_station(knobs, "wfm", false);
  CHECK_FALSE(knobs.squelch_on);
  CHECK(knobs.squelch_db == doctest::Approx(-32.0f));
  vocatron::toggle_squelch(knobs);
  CHECK(knobs.squelch_on);
  CHECK(vocatron::step_squelch(knobs, -1));
  CHECK(knobs.squelch_db == doctest::Approx(-34.0f));
  // Same station again (an LNA step): the hand-set level stays.
  vocatron::enter_station(knobs, "wfm", true);
  CHECK(knobs.squelch_db == doctest::Approx(-34.0f));
  // Another station: back to that mode's level, gate on.
  vocatron::toggle_squelch(knobs);
  CHECK_FALSE(knobs.squelch_on);
  vocatron::enter_station(knobs, "am", false);
  CHECK(knobs.squelch_on);
  CHECK(knobs.squelch_db == doctest::Approx(-52.0f));

  knobs.squelch_db = -120.0f;
  CHECK_FALSE(vocatron::step_squelch(knobs, -1));
  knobs.squelch_db = 0.0f;
  CHECK_FALSE(vocatron::step_squelch(knobs, +1));
}

TEST_CASE("signal bar and amp guard") {
  CHECK(vocatron::signal_fill(-200.0f) == doctest::Approx(0.0f));
  CHECK(vocatron::signal_fill(-45.0f) == doctest::Approx(0.5f));
  CHECK(vocatron::signal_fill(10.0f) == doctest::Approx(1.0f));

  vocatron::Knobs knobs;
  CHECK_FALSE(vocatron::amp_should_drop(knobs, 5.0f));
  knobs.amp = true;
  CHECK_FALSE(vocatron::amp_should_drop(knobs, 0.0f));
  CHECK(vocatron::amp_should_drop(knobs, 1.0f));
}
