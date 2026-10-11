#pragma once

// Key logic for the station screen. No radio and no terminal, so tests can
// press the same keys the screen does.

#include <cstdint>
#include <string>

namespace vocatron {

enum class TuiKey {
  None,
  Up,
  Down,
  Enter,
  Quit,
  Stop,
  SqlToggle,
  SqlLoosen,
  SqlTighten,
  Mute,
  Group,
  LnaDown,
  LnaUp,
};

enum class TuiAction { Stay, Quit, Listen, Retune, Idle };

struct TuiModel {
  int count = 0;
  int selected = 0;
  int playing = -1;
  bool listening = false;
  float squelch_db = -40.0f;
  bool squelch_on = true;
  bool hear = true;
  int groups = 1;
  int group = 0;
  int lna = 16;
};

inline TuiAction apply_tui_key(TuiModel &model, TuiKey key) {
  switch (key) {
  case TuiKey::Quit:
    return TuiAction::Quit;
  case TuiKey::Stop:
    if (!model.listening) {
      return TuiAction::Stay;
    }
    model.listening = false;
    model.playing = -1;
    return TuiAction::Idle;
  case TuiKey::Up:
    if (model.count > 0) {
      model.selected = (model.selected + model.count - 1) % model.count;
    }
    return TuiAction::Stay;
  case TuiKey::Down:
    if (model.count > 0) {
      model.selected = (model.selected + 1) % model.count;
    }
    return TuiAction::Stay;
  case TuiKey::Enter:
    if (model.count <= 0) {
      return TuiAction::Stay;
    }
    if (model.listening && model.playing == model.selected) {
      return TuiAction::Stay;
    }
    {
      const bool was = model.listening;
      model.playing = model.selected;
      model.listening = true;
      return was ? TuiAction::Retune : TuiAction::Listen;
    }
  case TuiKey::SqlToggle:
    model.squelch_on = !model.squelch_on;
    return TuiAction::Stay;
  case TuiKey::SqlLoosen:
    model.squelch_db -= 2.0f;
    if (model.squelch_db < -120.0f) {
      model.squelch_db = -120.0f;
    }
    return TuiAction::Stay;
  case TuiKey::SqlTighten:
    model.squelch_db += 2.0f;
    if (model.squelch_db > 0.0f) {
      model.squelch_db = 0.0f;
    }
    return TuiAction::Stay;
  case TuiKey::Mute:
    model.hear = !model.hear;
    return TuiAction::Stay;
  case TuiKey::Group:
    if (model.groups > 0) {
      model.group = (model.group + 1) % model.groups;
    }
    model.selected = 0;
    return TuiAction::Stay;
  case TuiKey::LnaDown:
    if (model.lna >= 8) {
      model.lna -= 8;
      return model.listening ? TuiAction::Retune : TuiAction::Stay;
    }
    return TuiAction::Stay;
  case TuiKey::LnaUp:
    if (model.lna <= 32) {
      model.lna += 8;
      return model.listening ? TuiAction::Retune : TuiAction::Stay;
    }
    return TuiAction::Stay;
  case TuiKey::None:
    return TuiAction::Stay;
  }
  return TuiAction::Stay;
}

// Bar from -90 dBFS to 0. '#' is the channel. '|' is the squelch mark.
inline std::string level_meter(float channel_dbfs, float mark_dbfs, int width) {
  if (width < 3) {
    width = 3;
  }
  if (width > 64) {
    width = 64;
  }
  auto slot = [width](float db) {
    float t = (db + 90.0f) / 90.0f;
    if (t < 0.0f) {
      t = 0.0f;
    }
    if (t > 1.0f) {
      t = 1.0f;
    }
    int index = static_cast<int>(t * static_cast<float>(width - 1) + 0.5f);
    if (index < 0) {
      index = 0;
    }
    if (index >= width) {
      index = width - 1;
    }
    return index;
  };
  std::string bar(static_cast<std::size_t>(width), '.');
  if (channel_dbfs > -190.0f) {
    const int level = slot(channel_dbfs);
    for (int i = 0; i <= level; ++i) {
      bar[static_cast<std::size_t>(i)] = '#';
    }
  }
  bar[static_cast<std::size_t>(slot(mark_dbfs))] = '|';
  return bar;
}

} // namespace vocatron
