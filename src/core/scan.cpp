#include "scan.hpp"

#include "dsp.hpp"
#include "hackrf_session.hpp"
#include "ring.hpp"
#include "rx.hpp"

#include <fftw3.h>
#include <hackrf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

namespace vocatron {
namespace {

constexpr std::uint32_t kSampleRateHz = 2'400'000;
constexpr std::uint32_t kBasebandRequestHz = 1'750'000;
constexpr std::uint64_t kLoOffsetHz = 250'000;
constexpr std::uint64_t kGridHz = 100'000;
// Survey: the tuner steps 0.5 MHz. Each step measures channels 150 to
// 850 kHz from the tuned frequency on both sides, which keeps clear of the
// DC spike in the middle and the filter edges, and covers every channel twice.
constexpr std::uint64_t kSurveyStepHz = 500'000;
constexpr double kNearHz = 150'000.0;
constexpr double kFarHz = 850'000.0;
// A station's power is measured over ±75 kHz of its centre.
constexpr double kHalfWidthHz = 75'000.0;
constexpr int kFftSize = 2048;
// After moving the tuner, drop 30 ms, then average 50 ms.
constexpr std::size_t kSettleBytes = 144'000;
constexpr std::size_t kSurveyBytes = 240'000;
// The floor is this quantile of all channel powers: the quieter channels.
constexpr double kFloorQuantile = 0.2;
// Verify: stations 200 to 800 kHz from the tuned frequency are decoded
// from one recording. Inside 200 kHz the DC spike gets in the way.
constexpr double kVerifyNearHz = 200'000.0;
constexpr double kVerifyFarHz = 800'000.0;

int8_t as_int8(std::uint8_t byte) {
  return byte < 128 ? static_cast<int8_t>(byte) : static_cast<int8_t>(static_cast<int>(byte) - 256);
}

class Fft {
public:
  Fft()
      : in_(static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * kFftSize))),
        out_(static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * kFftSize))),
        plan_(fftwf_plan_dft_1d(kFftSize, in_, out_, FFTW_FORWARD, FFTW_ESTIMATE)),
        window_(kFftSize), power_(kFftSize) {
    for (int i = 0; i < kFftSize; ++i) {
      window_[static_cast<std::size_t>(i)] =
          0.5f - 0.5f * std::cos(2.0f * 3.14159265f * static_cast<float>(i) / (kFftSize - 1));
    }
  }
  ~Fft() {
    if (plan_ != nullptr) {
      fftwf_destroy_plan(plan_);
    }
    fftwf_free(in_);
    fftwf_free(out_);
  }
  Fft(const Fft &) = delete;
  Fft &operator=(const Fft &) = delete;

  bool ok() const { return plan_ != nullptr; }

  // Averaged power per bin, DC in the middle (bin kFftSize/2).
  const std::vector<double> &average(const std::uint8_t *iq, std::size_t bytes) {
    std::fill(power_.begin(), power_.end(), 0.0);
    const std::size_t frames = bytes / 2 / kFftSize;
    for (std::size_t f = 0; f < frames; ++f) {
      const std::uint8_t *p = iq + f * kFftSize * 2;
      for (int i = 0; i < kFftSize; ++i) {
        const float w = window_[static_cast<std::size_t>(i)];
        in_[i][0] = w * static_cast<float>(as_int8(p[2 * i])) / 128.0f;
        in_[i][1] = w * static_cast<float>(as_int8(p[2 * i + 1])) / 128.0f;
      }
      fftwf_execute(plan_);
      for (int i = 0; i < kFftSize; ++i) {
        const int shifted = (i + kFftSize / 2) % kFftSize;
        const double re = out_[i][0];
        const double im = out_[i][1];
        power_[static_cast<std::size_t>(shifted)] += re * re + im * im;
      }
    }
    return power_;
  }

private:
  fftwf_complex *in_;
  fftwf_complex *out_;
  fftwf_plan plan_;
  std::vector<float> window_;
  std::vector<double> power_;
};

// Pulls bytes off the queue. Returns false if nothing arrives for 2 s.
bool read_bytes(SpscRing<std::uint8_t> &ring, std::uint8_t *dst, std::size_t want) {
  std::size_t got = 0;
  auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (got < want) {
    const std::size_t n = ring.try_read(dst + got, want - got);
    got += n;
    if (n > 0) {
      give_up = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    } else if (std::chrono::steady_clock::now() > give_up) {
      return false;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  return true;
}

void drain(SpscRing<std::uint8_t> &ring, std::vector<std::uint8_t> &scratch) {
  while (ring.try_read(scratch.data(), scratch.size()) > 0) {
  }
}

bool tell(ScanProgress progress, void *user, float fraction, const char *what) {
  return progress == nullptr || progress(fraction, what, user);
}

} // namespace

std::string scan_hit_name(const ScanHit &hit) {
  if (!hit.rds_name.empty()) {
    return hit.rds_name;
  }
  char name[32];
  std::snprintf(name, sizeof name, "FM %.1f", static_cast<double>(hit.hz) / 1.0e6);
  return name;
}

int run_fm_scan(const ScanConfig &config, std::vector<ScanHit> *hits, ScanProgress progress,
                void *user) {
  hits->clear();
  if (config.lna_db > 40 || config.lna_db % 8 != 0 || config.vga_db > 26 ||
      config.vga_db % 2 != 0 || config.stop_hz <= config.start_hz) {
    std::fprintf(stderr, "scan: bad gain or band\n");
    return 1;
  }
  Fft fft;
  if (!fft.ok()) {
    return 1;
  }
  Session session;
  if (!session.ok()) {
    return 1;
  }
  hackrf_device *dev = session.get();
  const std::uint32_t baseband = hackrf_compute_baseband_filter_bw(kBasebandRequestHz);
  const std::uint64_t first_lo = config.start_hz - static_cast<std::uint64_t>(kFarHz);
  if (!rx_check("hackrf_set_sample_rate_manual",
                hackrf_set_sample_rate_manual(dev, kSampleRateHz, 1)) ||
      !rx_check("hackrf_set_baseband_filter_bandwidth",
                hackrf_set_baseband_filter_bandwidth(dev, baseband)) ||
      !rx_check("hackrf_set_freq", hackrf_set_freq(dev, first_lo)) ||
      !rx_check("hackrf_set_amp_enable", hackrf_set_amp_enable(dev, config.amp ? 1 : 0)) ||
      !rx_check("hackrf_set_lna_gain", hackrf_set_lna_gain(dev, config.lna_db)) ||
      !rx_check("hackrf_set_vga_gain", hackrf_set_vga_gain(dev, config.vga_db))) {
    return 1;
  }

  SpscRing<std::uint8_t> ring(1u << 22);
  IqSink sink;
  sink.ring = &ring;
  RxStream rx(dev);
  if (!rx.start(&sink)) {
    return 1;
  }
  std::vector<std::uint8_t> scratch(1u << 18);
  std::vector<std::uint8_t> block(kSurveyBytes);

  // --- 1. Survey -----------------------------------------------------------
  // Channel centre (Hz) -> summed linear power and how many times measured.
  std::map<std::uint64_t, std::pair<double, int>> channels;
  const double bin_hz = static_cast<double>(kSampleRateHz) / kFftSize;
  const std::uint64_t last_lo = config.stop_hz + static_cast<std::uint64_t>(kFarHz);
  const auto steps = static_cast<int>((last_lo - first_lo) / kSurveyStepHz) + 1;
  int step = 0;
  for (std::uint64_t lo = first_lo; lo <= last_lo; lo += kSurveyStepHz, ++step) {
    char what[64];
    std::snprintf(what, sizeof what, "looking around %.1f MHz", static_cast<double>(lo) / 1e6);
    if (!tell(progress, user, 0.25f * static_cast<float>(step) / static_cast<float>(steps), what)) {
      return 0;
    }
    if (!rx_check("hackrf_set_freq", hackrf_set_freq(dev, lo))) {
      return 1;
    }
    drain(ring, scratch);
    if (!read_bytes(ring, scratch.data(), kSettleBytes) ||
        !read_bytes(ring, block.data(), block.size())) {
      std::fprintf(stderr, "scan: the radio stopped sending samples\n");
      return 1;
    }
    const std::vector<double> &power = fft.average(block.data(), block.size());
    const std::uint64_t lowest = lo - static_cast<std::uint64_t>(kFarHz);
    for (std::uint64_t c = (lowest / kGridHz + 1) * kGridHz; c <= lo + kFarHz; c += kGridHz) {
      const double offset = static_cast<double>(c) - static_cast<double>(lo);
      if (std::fabs(offset) < kNearHz || std::fabs(offset) > kFarHz || c < config.start_hz ||
          c > config.stop_hz) {
        continue;
      }
      double sum = 0.0;
      int bins = 0;
      const int centre = kFftSize / 2 + static_cast<int>(std::lround(offset / bin_hz));
      const int half = static_cast<int>(kHalfWidthHz / bin_hz);
      for (int b = centre - half; b <= centre + half; ++b) {
        if (b >= 0 && b < kFftSize) {
          sum += power[static_cast<std::size_t>(b)];
          ++bins;
        }
      }
      if (bins > 0) {
        auto &slot = channels[c];
        slot.first += sum / bins;
        slot.second += 1;
      }
    }
  }

  std::vector<std::pair<std::uint64_t, float>> level; // centre, dB
  for (const auto &[c, slot] : channels) {
    level.emplace_back(c, static_cast<float>(10.0 * std::log10(slot.first / slot.second + 1e-30)));
  }
  if (level.size() < 5) {
    return 0;
  }
  std::vector<float> sorted;
  for (const auto &entry : level) {
    sorted.push_back(entry.second);
  }
  std::sort(sorted.begin(), sorted.end());
  const float floor = sorted[static_cast<std::size_t>(kFloorQuantile * (sorted.size() - 1))];

  std::vector<ScanHit> candidates;
  for (std::size_t i = 0; i < level.size(); ++i) {
    const float here = level[i].second;
    const float left = i > 0 ? level[i - 1].second : -1e9f;
    const float right = i + 1 < level.size() ? level[i + 1].second : -1e9f;
    if (here - floor >= config.min_level_db && here >= left && here > right) {
      ScanHit hit;
      hit.hz = level[i].first;
      hit.level_db = here - floor;
      candidates.push_back(hit);
    }
  }

  // --- 2. Verify -----------------------------------------------------------
  // One tune covers about 1.7 MHz, so record once and decode every
  // candidate in that window from the same recording.
  const auto record_bytes =
      static_cast<std::size_t>(config.max_listen_s * kSampleRateHz) * 2 / (1u << 18) * (1u << 18);
  std::vector<std::uint8_t> recording(record_bytes);
  std::vector<float> audio(8192);
  std::vector<bool> done(candidates.size(), false);
  std::size_t finished = 0;
  for (std::size_t first = 0; first < candidates.size(); ++first) {
    if (done[first]) {
      continue;
    }
    // Put the first unheard candidate at the low edge of the window.
    const std::uint64_t lo = candidates[first].hz + static_cast<std::uint64_t>(kVerifyFarHz);
    char what[64];
    std::snprintf(what, sizeof what, "listening around %.1f MHz", static_cast<double>(lo) / 1e6);
    const float fraction =
        0.25f + 0.75f * static_cast<float>(finished) / static_cast<float>(candidates.size());
    if (!tell(progress, user, fraction, what)) {
      break;
    }
    if (!rx_check("hackrf_set_freq", hackrf_set_freq(dev, lo))) {
      return 1;
    }
    drain(ring, scratch);
    if (!read_bytes(ring, scratch.data(), kSettleBytes) ||
        !read_bytes(ring, recording.data(), recording.size())) {
      std::fprintf(stderr, "scan: the radio stopped sending samples\n");
      return 1;
    }
    for (std::size_t j = first; j < candidates.size(); ++j) {
      const double offset = static_cast<double>(candidates[j].hz) - static_cast<double>(lo);
      if (offset > kVerifyFarHz) {
        break;
      }
      if (done[j] || std::fabs(offset) < kVerifyNearHz) {
        continue;
      }
      ScanHit &hit = candidates[j];
      WfmStream stream(kSampleRateHz, static_cast<float>(offset));
      for (std::size_t off = 0; off < recording.size(); off += 1u << 18) {
        stream.process(recording.data() + off, 1u << 18, audio.data(), audio.size());
      }
      hit.clarity = stream.clarity();
      hit.rds_name = stream.rds_name();
      hit.rds_pi = stream.rds_pi();
      done[j] = true;
      ++finished;
      if (hit.clarity >= config.min_clarity || !hit.rds_name.empty()) {
        hits->push_back(hit);
      }
    }
  }
  std::sort(hits->begin(), hits->end(),
            [](const ScanHit &a, const ScanHit &b) { return a.hz < b.hz; });
  tell(progress, user, 1.0f, "done");
  return 0;
}

} // namespace vocatron
