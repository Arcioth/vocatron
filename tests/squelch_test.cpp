#include "core/squelch.hpp"

#include <doctest.h>

#include <vector>

TEST_CASE("squelch stays open when it is off") {
  vocatron::Squelch gate(false, -40.0f, 48000);
  float sample = 0.5f;
  CHECK(gate.apply(&sample, 1, -100.0f));
  CHECK(sample == doctest::Approx(0.5f));
}

TEST_CASE("squelch mutes a weak channel and passes a strong one") {
  vocatron::Squelch gate(true, -40.0f, 48000);
  std::vector<float> quiet(1000, 0.8f);
  CHECK_FALSE(gate.apply(quiet.data(), quiet.size(), -70.0f));
  CHECK(quiet.back() == doctest::Approx(0.0f).epsilon(0.001));

  std::vector<float> loud(4800, 0.8f);
  CHECK(gate.apply(loud.data(), loud.size(), -20.0f));
  // 10 ms fade, then full level. 4800 samples is 100 ms.
  CHECK(loud.back() == doctest::Approx(0.8f).epsilon(0.02));
}

TEST_CASE("squelch hangs so a short gap is not cut") {
  vocatron::Squelch gate(true, -40.0f, 48000);
  std::vector<float> open_it(4800, 1.0f);
  REQUIRE(gate.apply(open_it.data(), open_it.size(), -10.0f));

  // 0.1 s is inside the 0.3 s hang.
  std::vector<float> gap(4800, 1.0f);
  CHECK(gate.apply(gap.data(), gap.size(), -80.0f));
  CHECK(gap.back() == doctest::Approx(1.0f).epsilon(0.02));

  // Longer than the hang that is left. The tail is silent.
  std::vector<float> gone(20000, 1.0f);
  CHECK_FALSE(gate.apply(gone.data(), gone.size(), -80.0f));
  CHECK(gone.back() == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("squelch follows a tighter level without a click of hiss") {
  vocatron::Squelch gate(true, -40.0f, 48000);
  std::vector<float> open_it(1000, 0.5f);
  CHECK(gate.apply(open_it.data(), open_it.size(), -30.0f));
  gate.set_level(true, -20.0f);
  std::vector<float> later(20000, 0.5f);
  CHECK_FALSE(gate.apply(later.data(), later.size(), -30.0f));
  CHECK(later.back() == doctest::Approx(0.0f).epsilon(0.02));
}
