#include "cli/tui_model.hpp"

#include <doctest.h>

TEST_CASE("level meter marks the channel and the squelch") {
  CHECK(vocatron::level_meter(-55.0f, -40.0f, 10) == "#####|....");
  CHECK(vocatron::level_meter(0.0f, -40.0f, 10) == "#####|####");
  CHECK(vocatron::level_meter(-200.0f, -40.0f, 10) == ".....|....");
}

TEST_CASE("station screen keys move, tune, and stop") {
  vocatron::TuiModel model;
  model.count = 3;
  model.groups = 4;
  model.listening = true;
  model.playing = 0;

  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::Down) == vocatron::TuiAction::Stay);
  CHECK(model.selected == 1);
  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::Enter) == vocatron::TuiAction::Retune);
  CHECK(model.playing == 1);

  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::Enter) == vocatron::TuiAction::Stay);

  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::SqlLoosen) == vocatron::TuiAction::Stay);
  CHECK(model.squelch_db == doctest::Approx(-42.0f));
  model.squelch_db = -120.0f;
  vocatron::apply_tui_key(model, vocatron::TuiKey::SqlLoosen);
  CHECK(model.squelch_db == doctest::Approx(-120.0f));
  model.squelch_db = 0.0f;
  vocatron::apply_tui_key(model, vocatron::TuiKey::SqlTighten);
  CHECK(model.squelch_db == doctest::Approx(0.0f));

  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::SqlToggle) == vocatron::TuiAction::Stay);
  CHECK_FALSE(model.squelch_on);
  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::Mute) == vocatron::TuiAction::Stay);
  CHECK_FALSE(model.hear);

  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::LnaUp) == vocatron::TuiAction::Retune);
  CHECK(model.lna == 24);
  model.lna = 40;
  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::LnaUp) == vocatron::TuiAction::Stay);

  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::Group) == vocatron::TuiAction::Stay);
  CHECK(model.group == 1);
  CHECK(model.selected == 0);

  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::Stop) == vocatron::TuiAction::Idle);
  CHECK_FALSE(model.listening);
  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::Enter) == vocatron::TuiAction::Listen);
  CHECK(model.listening);
  CHECK(vocatron::apply_tui_key(model, vocatron::TuiKey::Quit) == vocatron::TuiAction::Quit);
}
