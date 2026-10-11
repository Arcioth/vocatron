#pragma once

// The knobs on the station screen, as plain values. No radio and no
// terminal, so tests can turn the same knobs the screen does.

#include <string>

namespace vocatron {

struct Knobs {
  // LNA: the first amplifier, 0 to 40 dB in steps of 8. More LNA lifts a
  // weak station above the radio's own hiss.
  int lna = 40;
  // VGA: the second amplifier, after the mixer. 0 to 26 dB in steps of 2.
  // This firmware stops sending samples above 26.
  int vga = 26;
  // RF amp: about 11 dB more, right at the antenna. Off unless turned on.
  bool amp = false;
  bool squelch_on = true;
  float squelch_db = -52.0f;
  bool muted = false;
  // [ ] and the gate switch keep the person's squelch until the station changes.
  bool squelch_manual = false;
};

constexpr int kLnaMax = 40;
constexpr int kLnaStep = 8;
constexpr int kVgaMax = 26;
constexpr int kVgaStep = 2;
constexpr float kSquelchStep = 2.0f;
constexpr float kSquelchMin = -120.0f;
constexpr float kSquelchMax = 0.0f;

// Each returns true when the value changed.
inline bool step_lna(Knobs &knobs, int direction) {
  const int next = knobs.lna + direction * kLnaStep;
  if (next < 0 || next > kLnaMax) {
    return false;
  }
  knobs.lna = next;
  return true;
}

inline bool step_vga(Knobs &knobs, int direction) {
  const int next = knobs.vga + direction * kVgaStep;
  if (next < 0 || next > kVgaMax) {
    return false;
  }
  knobs.vga = next;
  return true;
}

inline bool step_squelch(Knobs &knobs, int direction) {
  float next = knobs.squelch_db + static_cast<float>(direction) * kSquelchStep;
  if (next < kSquelchMin) {
    next = kSquelchMin;
  }
  if (next > kSquelchMax) {
    next = kSquelchMax;
  }
  knobs.squelch_manual = true;
  if (next == knobs.squelch_db) {
    return false;
  }
  knobs.squelch_db = next;
  return true;
}

inline void toggle_squelch(Knobs &knobs) {
  knobs.squelch_on = !knobs.squelch_on;
  knobs.squelch_manual = true;
}

// Where the gate starts for each mode. Measured on this radio at the
// screen's default gain (LNA 40, VGA 26, amp off), 2026-10-11:
// AM: empty channels -60 to -61, ATIS -34. Narrow FM: empty -56 to -62.
// Wide FM: stations -30 to -36, about where empty FM noise sits, so a level
// gate cannot tell them apart. Broadcast never pauses, so its gate starts off.
inline float squelch_for_mode(const std::string &mode) {
  if (mode == "am") {
    return -52.0f;
  }
  if (mode == "wfm") {
    return -32.0f;
  }
  return -48.0f;
}

inline bool squelch_starts_on(const std::string &mode) { return mode != "wfm"; }

// A new station gets its mode's gate, unless the person set one by hand
// for the same station.
inline void enter_station(Knobs &knobs, const std::string &mode, bool same_station) {
  if (!same_station) {
    knobs.squelch_manual = false;
  }
  if (!knobs.squelch_manual) {
    knobs.squelch_on = squelch_starts_on(mode);
    knobs.squelch_db = squelch_for_mode(mode);
  }
}

// 0 to 1 for a signal bar from -90 dBFS (empty) to 0 dBFS (full scale).
inline float signal_fill(float channel_dbfs) {
  float fill = (channel_dbfs + 90.0f) / 90.0f;
  if (fill < 0.0f) {
    fill = 0.0f;
  }
  if (fill > 1.0f) {
    fill = 1.0f;
  }
  return fill;
}

// With the amp on, any clipping means the input is too strong for it.
// The screen turns the amp off by itself above this.
constexpr float kAmpClipLimitPct = 0.5f;

inline bool amp_should_drop(const Knobs &knobs, float clip_pct) {
  return knobs.amp && clip_pct > kAmpClipLimitPct;
}

} // namespace vocatron
