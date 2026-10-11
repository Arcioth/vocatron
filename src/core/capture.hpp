#pragma once

#include <cstdint>

namespace vocatron {

struct CaptureConfig {
  uint64_t channel_hz = 0;
  double seconds = 5.0;
  const char *out_path = "samples/fm.iq";
  uint32_t lna_db = 16;
  uint32_t vga_db = 20;
  bool amp = false;
};

// Record int8 I/Q from the HackRF and print where the energy is.
// Returns 0 when the file is written and the sample count is complete.
int run_capture(const CaptureConfig &config);

// Feed the spectrum math a known tone, with no radio. Returns 0 on success.
int run_selftest();

// Stream 2.4 MS/s through the ring and count drops. Returns 0 when none were dropped.
int run_ringtest(double seconds, uint32_t lna_db, uint32_t vga_db);

} // namespace vocatron
