#pragma once

#include <atomic>
#include <cstdint>

namespace vocatron {

// One snapshot of the receiver, for a status line or a screen.
struct ListenView {
  float channel_dbfs = -200.0f;
  float adc_dbfs = -120.0f;
  float clip_pct = 0.0f;
  bool sql_open = false;
  std::uint64_t dropped = 0;
  std::uint64_t underruns = 0;
  float demod_peak = 0.0f;
  double elapsed_s = 0.0;
  std::uint64_t usb_bytes = 0;
};

// Return false to stop this listen. The TUI uses this to draw and read keys.
using ListenTick = bool (*)(const ListenView &view, void *user);

// A station change asked for by the tick. The tick fills it in and sets
// `pending`. Right after the tick returns, run_listen moves the tuner and
// swaps the demodulator without closing the radio or the speakers, then
// clears `pending`. `mode` is read at that moment only.
struct ListenRetune {
  bool pending = false;
  std::uint64_t channel_hz = 0;
  // "wfm", "am", or "nfm".
  const char *mode = nullptr;
  std::uint32_t lna_db = 16;
};

struct ListenConfig {
  std::uint64_t channel_hz = 0;
  // Printed in the status line. For a typed frequency this is that text.
  const char *name = nullptr;
  // 0 means keep going until Ctrl-C.
  double seconds = 0;
  std::uint32_t lna_db = 16;
  std::uint32_t vga_db = 20;
  bool amp = false;
  const char *out_path = nullptr;
  bool speakers = true;
  // "wfm", "am", or "nfm".
  const char *mode = "wfm";
  // From the channel file. Null for a typed frequency.
  const char *group = nullptr;
  // Off unless set. Opens when the filtered channel is at or above this dBFS.
  bool squelch = false;
  float squelch_dbfs = -40.0f;
  // When set, the plain status line is not printed. The tick owns the screen.
  ListenTick on_tick = nullptr;
  void *tick_user = nullptr;
  // Live knobs. Null means the fields above stay as they were at the start.
  // Both squelch pointers are required together.
  std::atomic<bool> *squelch_enabled = nullptr;
  std::atomic<float> *squelch_level = nullptr;
  // False writes silence to the speaker without closing the device.
  std::atomic<bool> *pass_audio = nullptr;
  // Set when Ctrl-C or SIGTERM ended the run.
  bool *signal_stop = nullptr;
  // Station changes from the tick. Null means one station per run.
  // Not allowed while recording: the run stops with an error instead.
  ListenRetune *retune = nullptr;
};

// 0: known name. hz, name, mode, and group filled in.
//    mode and group may be null if the caller does not want them.
// 1: the channel file could not be read (message already printed).
// 2: not a known name.
int resolve_channel_name(const char *text, std::uint64_t *hz, const char **name, const char **mode,
                         const char **group);

// Tune, demodulate, and play. Returns 0 when the run finished
// and the sample queue dropped nothing.
int run_listen(const ListenConfig &config);

} // namespace vocatron
