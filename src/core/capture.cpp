// Capture raw I/Q and report where the energy sits.
//
// Each sample is a pair of numbers, I and Q. Together they describe the
// signal around the frequency the HackRF is tuned to. An FFT turns a
// block of samples into "how strong is each frequency". Averaging many
// blocks lets a real station stand out from hiss.
//
// The HackRF adds a fake spike exactly at the tuned frequency. We tune
// 250 kHz below the channel so that spike is not on top of the station.

#include "capture.hpp"

#include "hackrf_session.hpp"
#include "ring.hpp"

#include <fftw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

namespace vocatron {
namespace {

constexpr uint32_t kSampleRateHz = 2'400'000;
constexpr uint32_t kBasebandRequestHz = 1'750'000;
constexpr uint64_t kLoOffsetHz = 250'000;
constexpr int kFftSize = 8192;
constexpr double kChannelHalfHz = 100'000.0;
constexpr double kDcGuardHz = 5'000.0;
constexpr double kQuietOffsetHz = -600'000.0;
constexpr int kPeakCount = 5;

bool check_call(const char *what, int rc);

// The USB callback's only job is to push bytes into the ring.
struct Sink {
  SpscRing<uint8_t> *ring = nullptr;
  std::atomic<uint64_t> dropped{0};
  std::atomic<uint64_t> zero_xfers{0};
  std::atomic<uint64_t> usb_bytes{0};
};

// Only the USB thread calls this. Copy into the ring and return.
// No allocation, no logging, no libhackrf calls. A full ring drops bytes.
extern "C" int vocatron_rx_callback(hackrf_transfer *transfer) {
  auto *sink = static_cast<Sink *>(transfer->rx_ctx);
  if (transfer->valid_length <= 0) {
    sink->zero_xfers.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const auto n = static_cast<size_t>(transfer->valid_length);
  sink->usb_bytes.fetch_add(n, std::memory_order_relaxed);
  const size_t wrote = sink->ring->try_write(transfer->buffer, n);
  if (wrote < n) {
    sink->dropped.fetch_add(n - wrote, std::memory_order_relaxed);
  }
  return 0;
}

struct PumpResult {
  bool started = false;
  bool stalled = false;
  size_t bytes = 0;
  uint64_t dropped = 0;
  uint64_t zero_xfers = 0;
};

// USB thread produces, this function's thread consumes, until `bytes_wanted`
// bytes have been taken off the ring. `dest` may be null to count only.
PumpResult pump_rx(hackrf_device *dev, uint8_t *dest, size_t bytes_wanted) {
  // 4 MiB holds about 0.8 s of 2.4 MS/s I/Q, far more than one USB burst.
  SpscRing<uint8_t> ring(1u << 22);
  Sink sink;
  sink.ring = &ring;

  std::atomic<size_t> stored{0};
  std::atomic<bool> producer_done{false};
  std::thread consumer([&] {
    std::vector<uint8_t> chunk(64 * 1024);
    while (true) {
      const size_t n = ring.try_read(chunk.data(), chunk.size());
      if (n == 0) {
        if (producer_done.load(std::memory_order_acquire)) {
          const size_t rest = ring.try_read(chunk.data(), chunk.size());
          if (rest == 0) {
            break;
          }
          // Fall through by looping; the bytes are put back only if we
          // don't copy them here. Copy them now.
          size_t have = stored.load(std::memory_order_relaxed);
          if (have < bytes_wanted) {
            const size_t room = bytes_wanted - have;
            const size_t take = rest < room ? rest : room;
            if (dest != nullptr && take > 0) {
              std::memcpy(dest + have, chunk.data(), take);
            }
            stored.store(have + take, std::memory_order_release);
          }
          continue;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      size_t have = stored.load(std::memory_order_relaxed);
      if (have < bytes_wanted) {
        const size_t room = bytes_wanted - have;
        const size_t take = n < room ? n : room;
        if (dest != nullptr && take > 0) {
          std::memcpy(dest + have, chunk.data(), take);
        }
        stored.store(have + take, std::memory_order_release);
      }
    }
  });

  PumpResult result;
  if (!check_call("hackrf_start_rx", hackrf_start_rx(dev, vocatron_rx_callback, &sink))) {
    producer_done.store(true, std::memory_order_release);
    consumer.join();
    return result;
  }
  result.started = true;

  const auto t0 = std::chrono::steady_clock::now();
  const double want_s = static_cast<double>(bytes_wanted / 2) / static_cast<double>(kSampleRateHz);
  const auto limit = std::chrono::milliseconds(static_cast<int>(want_s * 1000.0) + 3000);
  int last_mark = 0;
  while (stored.load(std::memory_order_acquire) < bytes_wanted) {
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    const size_t have = stored.load(std::memory_order_acquire);
    const int pct = static_cast<int>(have * 100 / bytes_wanted);
    if (pct / 20 > last_mark) {
      last_mark = pct / 20;
      std::printf("received %d%%\n", (pct / 20) * 20);
    }
    if (elapsed > std::chrono::seconds(1) && have == 0) {
      result.stalled = true;
      break;
    }
    const int streaming = hackrf_is_streaming(dev);
    if (streaming != HACKRF_TRUE && (have > 0 || elapsed > std::chrono::milliseconds(500))) {
      result.stalled = have == 0;
      if (streaming != HACKRF_ERROR_STREAMING_EXIT_CALLED) {
        print_error("hackrf_is_streaming", streaming);
      }
      break;
    }
    if (elapsed > limit) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const int stop_rc = hackrf_stop_rx(dev);
  if (stop_rc != HACKRF_SUCCESS && stop_rc != HACKRF_ERROR_STREAMING_STOPPED &&
      stop_rc != HACKRF_ERROR_STREAMING_EXIT_CALLED) {
    print_error("hackrf_stop_rx", stop_rc);
  }
  // stop_rx waits until the USB callback has finished, so the ring will not grow after this.
  producer_done.store(true, std::memory_order_release);
  consumer.join();

  result.bytes = stored.load(std::memory_order_acquire);
  result.dropped = sink.dropped.load(std::memory_order_relaxed);
  result.zero_xfers = sink.zero_xfers.load(std::memory_order_relaxed);
  return result;
}

int8_t as_int8(uint8_t byte) {
  return byte < 128 ? static_cast<int8_t>(byte) : static_cast<int8_t>(static_cast<int>(byte) - 256);
}

struct Peak {
  double hz = 0.0;
  float dbfs = -200.0f;
};

struct Report {
  bool ok = false;
  double clip_pct = 0.0;
  int peak_abs = 0;
  double dc_hz = 0.0;
  float dc_dbfs = -200.0f;
  float noise_dbfs = -200.0f;
  float channel_over_quiet_db = 0.0f;
  double quiet_hz = 0.0;
  Peak peaks[kPeakCount];
  int npeaks = 0;
  int blocks = 0;
};

class FftBuf {
public:
  explicit FftBuf(int n)
      : p_(static_cast<fftwf_complex *>(
            fftwf_malloc(sizeof(fftwf_complex) * static_cast<size_t>(n)))) {}

  ~FftBuf() { fftwf_free(p_); }

  FftBuf(const FftBuf &) = delete;
  FftBuf &operator=(const FftBuf &) = delete;

  fftwf_complex *get() const { return p_; }

private:
  fftwf_complex *p_;
};

class FftPlan {
public:
  FftPlan(int n, fftwf_complex *in, fftwf_complex *out)
      : plan_(fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE)) {}

  ~FftPlan() {
    if (plan_ != nullptr) {
      fftwf_destroy_plan(plan_);
    }
  }

  FftPlan(const FftPlan &) = delete;
  FftPlan &operator=(const FftPlan &) = delete;

  void run() const { fftwf_execute(plan_); }
  bool ok() const { return plan_ != nullptr; }

private:
  fftwf_plan plan_;
};

double bin_offset_hz(int bin, uint32_t sample_rate_hz) {
  int signed_bin = bin;
  if (bin >= kFftSize / 2) {
    signed_bin = bin - kFftSize;
  }
  return static_cast<double>(signed_bin) * static_cast<double>(sample_rate_hz) / kFftSize;
}

Report analyze(const uint8_t *iq, size_t bytes, uint32_t sample_rate_hz, uint64_t lo_hz,
               uint64_t channel_hz) {
  Report report;
  const size_t samples = bytes / 2;
  const size_t block_samples = static_cast<size_t>(kFftSize);
  report.blocks = static_cast<int>(samples / block_samples);
  if (report.blocks < 1) {
    return report;
  }

  uint64_t clipped = 0;
  int peak_abs = 0;
  const size_t used_bytes = static_cast<size_t>(report.blocks) * block_samples * 2;
  for (size_t i = 0; i < used_bytes; ++i) {
    const int8_t s = as_int8(iq[i]);
    const int mag = s < 0 ? -static_cast<int>(s) : static_cast<int>(s);
    peak_abs = std::max(peak_abs, mag);
    if (s <= -127 || s >= 127) {
      ++clipped;
    }
  }
  report.clip_pct = 100.0 * static_cast<double>(clipped) / static_cast<double>(used_bytes);
  report.peak_abs = peak_abs;

  std::vector<float> window(block_samples);
  for (size_t n = 0; n < block_samples; ++n) {
    const double phase = 2.0 * 3.14159265358979323846 * static_cast<double>(n) / kFftSize;
    window[n] = static_cast<float>(0.5 * (1.0 - std::cos(phase)));
  }

  FftBuf in(kFftSize);
  FftBuf out(kFftSize);
  if (in.get() == nullptr || out.get() == nullptr) {
    return report;
  }
  FftPlan plan(kFftSize, in.get(), out.get());
  if (!plan.ok()) {
    return report;
  }

  std::vector<double> accum(block_samples, 0.0);
  for (int block = 0; block < report.blocks; ++block) {
    const size_t base = static_cast<size_t>(block) * block_samples * 2;
    for (int n = 0; n < kFftSize; ++n) {
      const float i = static_cast<float>(as_int8(iq[base + static_cast<size_t>(2 * n)])) / 128.0f;
      const float q =
          static_cast<float>(as_int8(iq[base + static_cast<size_t>(2 * n + 1)])) / 128.0f;
      in.get()[n][0] = i * window[static_cast<size_t>(n)];
      in.get()[n][1] = q * window[static_cast<size_t>(n)];
    }
    plan.run();
    for (int k = 0; k < kFftSize; ++k) {
      const float re = out.get()[k][0];
      const float im = out.get()[k][1];
      accum[static_cast<size_t>(k)] += static_cast<double>(re) * re + static_cast<double>(im) * im;
    }
  }

  // A full-scale complex tone (amplitude 1) lands near 0 dBFS.
  // The Hann window's average height is 1/2, so undo that here.
  std::vector<float> dbfs(block_samples);
  const double scale = 2.0 / static_cast<double>(kFftSize);
  for (int k = 0; k < kFftSize; ++k) {
    const double mean = accum[static_cast<size_t>(k)] / report.blocks;
    const double amplitude = scale * std::sqrt(mean);
    dbfs[static_cast<size_t>(k)] =
        static_cast<float>(20.0 * std::log10(std::max(amplitude, 1e-20)));
  }

  const double lo = static_cast<double>(lo_hz);
  report.dc_hz = lo;
  report.dc_dbfs = dbfs[0];

  std::vector<float> noise;
  noise.reserve(block_samples);
  for (int k = 0; k < kFftSize; ++k) {
    if (std::fabs(bin_offset_hz(k, sample_rate_hz)) >= kDcGuardHz) {
      noise.push_back(dbfs[static_cast<size_t>(k)]);
    }
  }
  if (!noise.empty()) {
    const size_t mid = noise.size() / 2;
    std::nth_element(noise.begin(), noise.begin() + static_cast<std::ptrdiff_t>(mid), noise.end());
    report.noise_dbfs = noise[mid];
  }

  std::vector<char> used(block_samples, 0);
  for (int k = 0; k < kFftSize; ++k) {
    if (std::fabs(bin_offset_hz(k, sample_rate_hz)) < kDcGuardHz) {
      used[static_cast<size_t>(k)] = 1;
    }
  }

  const double bin_hz = static_cast<double>(sample_rate_hz) / kFftSize;
  const int suppress = static_cast<int>(std::ceil(kChannelHalfHz / bin_hz));
  for (int p = 0; p < kPeakCount; ++p) {
    int best = -1;
    float best_db = -1.0e9f;
    for (int k = 0; k < kFftSize; ++k) {
      if (used[static_cast<size_t>(k)] != 0) {
        continue;
      }
      if (dbfs[static_cast<size_t>(k)] > best_db) {
        best_db = dbfs[static_cast<size_t>(k)];
        best = k;
      }
    }
    if (best < 0) {
      break;
    }
    report.peaks[p].hz = lo + bin_offset_hz(best, sample_rate_hz);
    report.peaks[p].dbfs = best_db;
    ++report.npeaks;
    for (int k = std::max(0, best - suppress); k <= std::min(kFftSize - 1, best + suppress); ++k) {
      used[static_cast<size_t>(k)] = 1;
    }
  }

  auto sum_window = [&](double center_offset_hz) {
    double sum = 0.0;
    for (int k = 0; k < kFftSize; ++k) {
      const double off = bin_offset_hz(k, sample_rate_hz);
      if (std::fabs(off - center_offset_hz) <= kChannelHalfHz) {
        sum += accum[static_cast<size_t>(k)];
      }
    }
    return sum;
  };

  const double channel_offset = static_cast<double>(channel_hz) - lo;
  const double channel_power = sum_window(channel_offset);
  // The comparison slice is the quietest equal-width piece of this
  // capture, skipping the channel itself and the DC spike. A fixed
  // offset can land on another bump and hide a real station.
  const double edge = static_cast<double>(sample_rate_hz) / 2.0 - kChannelHalfHz;
  const double step = bin_hz * 40.0;
  double quiet_power = 0.0;
  double quiet_offset = 0.0;
  bool have_quiet = false;
  for (double off = -edge; off <= edge; off += step) {
    const bool hits_dc = std::fabs(off) < kChannelHalfHz + kDcGuardHz;
    const bool hits_channel = std::fabs(off - channel_offset) < (2.0 * kChannelHalfHz);
    if (hits_dc || hits_channel) {
      continue;
    }
    const double power = sum_window(off);
    if (!have_quiet || power < quiet_power) {
      quiet_power = power;
      quiet_offset = off;
      have_quiet = true;
    }
  }
  if (!have_quiet) {
    quiet_power = sum_window(kQuietOffsetHz);
    quiet_offset = kQuietOffsetHz;
  }
  report.quiet_hz = lo + quiet_offset;
  report.channel_over_quiet_db = static_cast<float>(
      10.0 * std::log10(std::max(channel_power, 1e-20) / std::max(quiet_power, 1e-20)));
  report.ok = true;
  return report;
}

bool check_call(const char *what, int rc) {
  if (rc == HACKRF_SUCCESS) {
    return true;
  }
  print_error(what, rc);
  return false;
}

void print_report(const Report &report, uint64_t channel_hz) {
  std::printf("clipped %.3f%% of samples, loudest sample %d out of 127\n", report.clip_pct,
              report.peak_abs);
  std::printf("DC spike at %.3f MHz: %.1f dBFS\n", report.dc_hz / 1.0e6, report.dc_dbfs);
  std::printf("noise floor (median bin): %.1f dBFS\n", report.noise_dbfs);
  std::printf("strongest narrow bins (each is only about 300 Hz wide):\n");
  if (report.npeaks < 1) {
    std::printf("no peak outside the DC spike\n");
  }
  for (int i = 0; i < report.npeaks; ++i) {
    const double khz = (report.peaks[i].hz - static_cast<double>(channel_hz)) / 1.0e3;
    std::printf(
        "narrow bin %d: %.3f MHz  %+.1f kHz from channel  %.1f dBFS  (%.1f dB over noise)\n", i + 1,
        report.peaks[i].hz / 1.0e6, khz, report.peaks[i].dbfs,
        report.peaks[i].dbfs - report.noise_dbfs);
  }
  std::printf("channel %.3f MHz is %.1f dB hotter than a quiet slice at %.3f MHz\n",
              static_cast<double>(channel_hz) / 1.0e6, report.channel_over_quiet_db,
              report.quiet_hz / 1.0e6);

  const bool on_channel =
      report.npeaks > 0 &&
      std::fabs(report.peaks[0].hz - static_cast<double>(channel_hz)) <= 150'000.0;
  const bool channel_hot = report.channel_over_quiet_db >= 8.0f;
  if (report.clip_pct > 5.0) {
    std::printf("Clipped hard. The signal is too strong. Try again with --lna 8.\n");
  } else if (channel_hot && on_channel) {
    std::printf("A station is on this channel.\n");
  } else if (channel_hot) {
    std::printf("This channel is louder than the quiet slice.\n");
  } else if (report.npeaks > 0 && report.peaks[0].dbfs - report.noise_dbfs >= 10.0f) {
    std::printf("Energy is in the window, but it is not piled on this channel.\n");
  } else {
    std::printf("Nothing stands out above the noise.\n");
  }
  if (report.peak_abs < 12 && report.clip_pct < 1.0) {
    std::printf("The loudest sample is only %d counts. The 8-bit converter is barely moving.\n"
                "Raise the gain, for example --lna 40 --vga 26. Leave the RF amp off.\n",
                report.peak_abs);
  }
}

} // namespace

int run_selftest() {
  // A pure tone exactly on FFT bin 256, which is +75 kHz at 2.4 MS/s.
  constexpr int kBlocks = 16;
  constexpr double kOffsetHz = 75'000.0;
  constexpr uint64_t kLo = 99'750'000;
  constexpr uint64_t kChannel = kLo + static_cast<uint64_t>(kOffsetHz);
  constexpr float kAmplitude = 0.5f;

  const size_t samples = static_cast<size_t>(kBlocks) * static_cast<size_t>(kFftSize);
  std::vector<uint8_t> iq(samples * 2);
  for (size_t n = 0; n < samples; ++n) {
    const double phase = 2.0 * 3.14159265358979323846 * kOffsetHz * static_cast<double>(n) /
                         static_cast<double>(kSampleRateHz);
    const int i = static_cast<int>(std::lround(kAmplitude * std::cos(phase) * 128.0));
    const int q = static_cast<int>(std::lround(kAmplitude * std::sin(phase) * 128.0));
    iq[2 * n] = static_cast<uint8_t>(static_cast<int8_t>(i));
    iq[2 * n + 1] = static_cast<uint8_t>(static_cast<int8_t>(q));
  }

  const Report report = analyze(iq.data(), iq.size(), kSampleRateHz, kLo, kChannel);
  const double expected = static_cast<double>(kChannel);
  const double bin_hz = static_cast<double>(kSampleRateHz) / kFftSize;
  if (!report.ok || report.npeaks < 1 || std::fabs(report.peaks[0].hz - expected) > bin_hz * 0.51 ||
      report.peaks[0].dbfs < -8.0f || report.peaks[0].dbfs > -4.0f ||
      report.channel_over_quiet_db < 20.0f) {
    std::fprintf(stderr, "selftest failed: peak %.3f MHz %.1f dBFS, channel delta %.1f dB\n",
                 report.npeaks > 0 ? report.peaks[0].hz / 1.0e6 : 0.0,
                 report.npeaks > 0 ? report.peaks[0].dbfs : -200.0f, report.channel_over_quiet_db);
    return 1;
  }
  std::printf("selftest ok: tone at %.3f MHz, %.1f dBFS\n", report.peaks[0].hz / 1.0e6,
              report.peaks[0].dbfs);
  return 0;
}

int run_capture(const CaptureConfig &config) {
  if (config.channel_hz <= kLoOffsetHz) {
    std::fprintf(stderr, "frequency is too low for the 250 kHz tune offset\n");
    return 1;
  }
  if (config.seconds < 0.2 || config.seconds > 30.0) {
    std::fprintf(stderr, "--seconds must be between 0.2 and 30\n");
    return 1;
  }
  if (config.lna_db > 40 || config.lna_db % 8 != 0) {
    std::fprintf(stderr, "--lna must be 0, 8, 16, 24, 32, or 40\n");
    return 1;
  }
  if (config.vga_db > 26 || config.vga_db % 2 != 0) {
    std::fprintf(
        stderr,
        "--vga must be an even number from 0 to 26\n"
        "VGA 28 and above can make this firmware accept the setting and then send no samples.\n");
    return 1;
  }

  const uint64_t samples = static_cast<uint64_t>(std::llround(config.seconds * kSampleRateHz));
  const uint64_t bytes64 = samples * 2;
  std::vector<uint8_t> buffer(static_cast<size_t>(bytes64));

  Session session;
  if (!session.ok()) {
    return 1;
  }
  if (print_identity(session) != 0) {
    return 1;
  }

  const uint64_t lo_hz = config.channel_hz - kLoOffsetHz;
  const uint32_t baseband = hackrf_compute_baseband_filter_bw(kBasebandRequestHz);
  std::printf(
      "tune: channel %.3f MHz, LO %.3f MHz (250 kHz below, so the DC spike misses the station)\n",
      static_cast<double>(config.channel_hz) / 1.0e6, static_cast<double>(lo_hz) / 1.0e6);
  std::printf("rate %.3f MS/s, baseband %.3f MHz, amp %s, LNA %u dB, VGA %u dB\n",
              kSampleRateHz / 1.0e6, baseband / 1.0e6, config.amp ? "ON" : "off", config.lna_db,
              config.vga_db);
  if (config.amp) {
    std::printf("RF amp is ON. A strong station can overload the receiver.\n");
  }

  hackrf_device *dev = session.get();
  if (!check_call("hackrf_set_sample_rate_manual",
                  hackrf_set_sample_rate_manual(dev, kSampleRateHz, 1)) ||
      !check_call("hackrf_set_baseband_filter_bandwidth",
                  hackrf_set_baseband_filter_bandwidth(dev, baseband)) ||
      !check_call("hackrf_set_freq", hackrf_set_freq(dev, lo_hz)) ||
      !check_call("hackrf_set_amp_enable", hackrf_set_amp_enable(dev, config.amp ? 1 : 0)) ||
      !check_call("hackrf_set_lna_gain", hackrf_set_lna_gain(dev, config.lna_db)) ||
      !check_call("hackrf_set_vga_gain", hackrf_set_vga_gain(dev, config.vga_db))) {
    return 1;
  }

  const PumpResult pumped = pump_rx(dev, buffer.data(), buffer.size());
  const size_t got = pumped.bytes;
  if (!pumped.started || pumped.stalled || got == 0) {
    std::fprintf(stderr,
                 "Streaming produced no samples.\n"
                 "On this firmware, VGA 28 and above can accept the gain and then send nothing.\n"
                 "This run used LNA %u dB and VGA %u dB, with the RF amp %s.\n",
                 config.lna_db, config.vga_db, config.amp ? "ON" : "off");
    return 1;
  }
  if (got * 10 < buffer.size() * 9) {
    std::fprintf(stderr, "Short capture: got %zu of %zu bytes.\n", got, buffer.size());
    return 1;
  }
  if (pumped.dropped != 0) {
    std::fprintf(stderr, "Ring dropped %llu bytes. The DSP side did not keep up.\n",
                 static_cast<unsigned long long>(pumped.dropped));
    return 1;
  }

  std::filesystem::path out(config.out_path);
  std::error_code ec;
  if (out.has_parent_path()) {
    std::filesystem::create_directories(out.parent_path(), ec);
  }
  if (ec) {
    std::fprintf(stderr, "could not create %s: %s\n", out.parent_path().c_str(),
                 ec.message().c_str());
    return 1;
  }

  FILE *file = std::fopen(config.out_path, "wb");
  if (file == nullptr) {
    std::fprintf(stderr, "could not open %s for writing\n", config.out_path);
    return 1;
  }
  const size_t wrote = std::fwrite(buffer.data(), 1, got, file);
  std::fclose(file);
  if (wrote != got) {
    std::fprintf(stderr, "short write to %s (%zu of %zu)\n", config.out_path, wrote, got);
    return 1;
  }

  const double duration = static_cast<double>(got / 2) / static_cast<double>(kSampleRateHz);
  std::printf("wrote %s (%zu bytes, %.2f s)\n", config.out_path, got, duration);
  std::printf("dropped %llu bytes, %llu empty USB transfers\n",
              static_cast<unsigned long long>(pumped.dropped),
              static_cast<unsigned long long>(pumped.zero_xfers));

  const Report report = analyze(buffer.data(), got, kSampleRateHz, lo_hz, config.channel_hz);
  if (!report.ok) {
    std::fprintf(stderr, "not enough samples to measure a spectrum\n");
    return 1;
  }
  print_report(report, config.channel_hz);
  return 0;
}

int run_ringtest(double seconds, uint32_t lna_db, uint32_t vga_db) {
  if (seconds < 0.2 || seconds > 30.0) {
    std::fprintf(stderr, "--seconds must be between 0.2 and 30\n");
    return 1;
  }
  if (lna_db > 40 || lna_db % 8 != 0) {
    std::fprintf(stderr, "--lna must be 0, 8, 16, 24, 32, or 40\n");
    return 1;
  }
  if (vga_db > 26 || vga_db % 2 != 0) {
    std::fprintf(stderr, "--vga must be an even number from 0 to 26\n");
    return 1;
  }

  // The whip is at about 17 cm, so this tune is in the band that length matches.
  // The test only checks that every sample survives the ring.
  constexpr uint64_t kChannelHz = 439'150'000;
  const uint64_t sample_count = static_cast<uint64_t>(std::llround(seconds * kSampleRateHz));
  const size_t bytes_wanted = static_cast<size_t>(sample_count * 2);

  Session session;
  if (!session.ok()) {
    return 1;
  }
  if (print_identity(session) != 0) {
    return 1;
  }

  const uint64_t lo_hz = kChannelHz - kLoOffsetHz;
  const uint32_t baseband = hackrf_compute_baseband_filter_bw(kBasebandRequestHz);
  std::printf("ring test: %.3f MHz for %.2f s, amp off, LNA %u dB, VGA %u dB\n",
              static_cast<double>(kChannelHz) / 1.0e6, seconds, lna_db, vga_db);
  std::printf("rate %.3f MS/s, baseband %.3f MHz. Samples are counted, not saved.\n",
              kSampleRateHz / 1.0e6, baseband / 1.0e6);

  hackrf_device *dev = session.get();
  if (!check_call("hackrf_set_sample_rate_manual",
                  hackrf_set_sample_rate_manual(dev, kSampleRateHz, 1)) ||
      !check_call("hackrf_set_baseband_filter_bandwidth",
                  hackrf_set_baseband_filter_bandwidth(dev, baseband)) ||
      !check_call("hackrf_set_freq", hackrf_set_freq(dev, lo_hz)) ||
      !check_call("hackrf_set_amp_enable", hackrf_set_amp_enable(dev, 0)) ||
      !check_call("hackrf_set_lna_gain", hackrf_set_lna_gain(dev, lna_db)) ||
      !check_call("hackrf_set_vga_gain", hackrf_set_vga_gain(dev, vga_db))) {
    return 1;
  }

  const PumpResult pumped = pump_rx(dev, nullptr, bytes_wanted);
  if (!pumped.started || pumped.stalled || pumped.bytes == 0) {
    std::fprintf(stderr, "Streaming produced no samples.\n");
    return 1;
  }
  const double duration =
      static_cast<double>(pumped.bytes / 2) / static_cast<double>(kSampleRateHz);
  std::printf("counted %zu bytes (%.2f s)\n", pumped.bytes, duration);
  std::printf("dropped %llu bytes, %llu empty USB transfers\n",
              static_cast<unsigned long long>(pumped.dropped),
              static_cast<unsigned long long>(pumped.zero_xfers));
  if (pumped.dropped != 0 || pumped.bytes * 10 < bytes_wanted * 9) {
    std::fprintf(stderr, "Ring did not keep every sample at 2.4 MS/s.\n");
    return 1;
  }
  std::printf("Ring kept up. No samples dropped.\n");
  return 0;
}

} // namespace vocatron
