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
// Band search: 20 MS/s through a 15 MHz filter. Each tuner position
// measures channels 100 kHz to 7 MHz above it (clear of the DC spike in
// the middle); the next position is 6.8 MHz higher.
constexpr std::uint32_t kWideRateHz = 20'000'000;
constexpr std::uint32_t kWideBasebandHz = 15'000'000;
constexpr int kWideFftSize = 16384; // 1.2 kHz bins
constexpr int kWideFrames = 16;     // 13 ms looked at per position
constexpr double kWideNearHz = 100'000.0;
constexpr double kWideFarHz = 7'000'000.0;
constexpr double kWideStepHz = 6'800'000.0;
// The first two seconds after the stream starts read hot (seen live as
// several false "talk" hits right at the start); they are not kept.
constexpr double kWarmupS = 2.0;
// After moving the tuner, drop 10 ms.
constexpr std::size_t kWideSettleBytes = 400'000;
// A visit counts as "above usual" this far over the channel's median.
constexpr float kTalkJumpDb = 6.0f;
// Talking: the loudest visit at least this far over the usual level,
// and at least this many visits above usual.
constexpr float kTalkPeakDb = 10.0f;
constexpr int kTalkMinTimes = 2;
// Steady: the usual level at least this far over the band floor.
constexpr float kSteadyDb = 8.0f;
// Interference comb: steady carriers 100 kHz to 5 MHz apart, matching the
// spacing within 3 kHz, at least 5 times. A 608 kHz comb was seen at the
// desk across the whole airband.
constexpr double kCombMinHz = 100'000.0;
constexpr double kCombMaxHz = 5'000'000.0;
constexpr double kCombSlopHz = 3'000.0;
constexpr int kCombMinTeeth = 5;
// Whole-megahertz hits (the radio's clock): within 9 kHz, 3 or more of them.
constexpr std::uint64_t kWholeMhzSlopHz = 9'000;
constexpr std::size_t kWholeMhzMinHits = 3;
// Verify: stations 200 to 800 kHz from the tuned frequency are decoded
// from one recording. Inside 200 kHz the DC spike gets in the way.
constexpr double kVerifyNearHz = 200'000.0;
constexpr double kVerifyFarHz = 800'000.0;

int8_t as_int8(std::uint8_t byte) {
  return byte < 128 ? static_cast<int8_t>(byte) : static_cast<int8_t>(static_cast<int>(byte) - 256);
}

class Fft {
public:
  explicit Fft(int size)
      : size_(size), in_(static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * size))),
        out_(static_cast<fftwf_complex *>(fftwf_malloc(sizeof(fftwf_complex) * size))),
        plan_(fftwf_plan_dft_1d(size, in_, out_, FFTW_FORWARD, FFTW_ESTIMATE)),
        window_(static_cast<std::size_t>(size)), power_(static_cast<std::size_t>(size)) {
    for (int i = 0; i < size; ++i) {
      window_[static_cast<std::size_t>(i)] =
          0.5f - 0.5f * std::cos(2.0f * 3.14159265f * static_cast<float>(i) /
                                 static_cast<float>(size - 1));
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
  int size() const { return size_; }

  // Averaged power per bin, DC in the middle (bin size/2).
  const std::vector<double> &average(const std::uint8_t *iq, std::size_t bytes) {
    std::fill(power_.begin(), power_.end(), 0.0);
    const auto n = static_cast<std::size_t>(size_);
    const std::size_t frames = bytes / 2 / n;
    for (std::size_t f = 0; f < frames; ++f) {
      const std::uint8_t *p = iq + f * n * 2;
      for (std::size_t i = 0; i < n; ++i) {
        const float w = window_[i];
        in_[i][0] = w * static_cast<float>(as_int8(p[2 * i])) / 128.0f;
        in_[i][1] = w * static_cast<float>(as_int8(p[2 * i + 1])) / 128.0f;
      }
      fftwf_execute(plan_);
      for (std::size_t i = 0; i < n; ++i) {
        const std::size_t shifted = (i + n / 2) % n;
        const double re = out_[i][0];
        const double im = out_[i][1];
        power_[shifted] += re * re + im * im;
      }
    }
    return power_;
  }

private:
  int size_;
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

double gap_hz(std::uint64_t a, std::uint64_t b) {
  return a > b ? static_cast<double>(a - b) : static_cast<double>(b - a);
}

// Flag hits that look like interference rather than stations.
void mark_interference(std::vector<ScanHit> &hits, double grid_hz) {
  // Teeth snap to the channel grid, so allow one grid step of error.
  const double slop = kCombSlopHz + grid_hz;
  // 1. A comb: many steady carriers evenly spaced. Try each gap between
  //    neighbouring steady hits as the spacing, count how many pairs sit a
  //    whole number of spacings apart, and keep the best spacing.
  std::vector<std::size_t> steady;
  for (std::size_t i = 0; i < hits.size(); ++i) {
    if (hits[i].steady) {
      steady.push_back(i);
    }
  }
  double best_spacing = 0.0;
  int best_score = 0;
  for (std::size_t k = 1; k < steady.size(); ++k) {
    const double spacing = gap_hz(hits[steady[k]].hz, hits[steady[k - 1]].hz);
    if (spacing < kCombMinHz || spacing > kCombMaxHz) {
      continue;
    }
    int score = 0;
    for (std::size_t k2 = 1; k2 < steady.size(); ++k2) {
      const double gap = gap_hz(hits[steady[k2]].hz, hits[steady[k2 - 1]].hz);
      const double multiple = std::round(gap / spacing);
      if (multiple >= 1.0 && multiple <= 3.0 && std::fabs(gap - multiple * spacing) <= slop) {
        ++score;
      }
    }
    if (score > best_score) {
      best_score = score;
      best_spacing = spacing;
    }
  }
  if (best_score >= kCombMinTeeth) {
    // A hit is a tooth when another steady hit sits a whole number of
    // spacings away (up to 6, to bridge teeth that were not picked up).
    for (const std::size_t i : steady) {
      for (const std::size_t j : steady) {
        if (i == j) {
          continue;
        }
        const double gap = gap_hz(hits[i].hz, hits[j].hz);
        const double multiple = std::round(gap / best_spacing);
        if (multiple >= 1.0 && multiple <= 6.0 &&
            std::fabs(gap - multiple * best_spacing) <= slop * multiple) {
          hits[i].interference = true;
        }
      }
    }
  }
  // 2. Whole megahertz: several hits within a few kHz of x.000 MHz.
  std::vector<std::size_t> whole;
  for (std::size_t i = 0; i < hits.size(); ++i) {
    const std::uint64_t rest = hits[i].hz % 1'000'000;
    if (rest <= kWholeMhzSlopHz || rest >= 1'000'000 - kWholeMhzSlopHz) {
      whole.push_back(i);
    }
  }
  if (whole.size() >= kWholeMhzMinHits) {
    for (const std::size_t i : whole) {
      hits[i].interference = true;
    }
  }
}

bool tell(ScanProgress progress, void *user, float fraction, const char *what) {
  return progress == nullptr || progress(fraction, what, user);
}

} // namespace

std::vector<ScanHit> find_activity(const Band &band, const std::vector<std::vector<float>> &visits,
                                   double seconds) {
  const std::size_t count = visits.size();
  std::vector<float> usual(count, -200.0f);
  std::vector<float> loudest(count, -200.0f);
  std::vector<int> above(count, 0);
  std::vector<std::size_t> last_above(count, 0);
  for (std::size_t c = 0; c < count; ++c) {
    const std::vector<float> &v = visits[c];
    if (v.empty()) {
      continue;
    }
    std::vector<float> sorted = v;
    std::nth_element(sorted.begin(),
                     sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    usual[c] = sorted[sorted.size() / 2];
    for (std::size_t i = 0; i < v.size(); ++i) {
      loudest[c] = std::max(loudest[c], v[i]);
      if (v[i] >= usual[c] + kTalkJumpDb) {
        ++above[c];
        last_above[c] = i;
      }
    }
  }
  std::vector<float> heard;
  for (std::size_t c = 0; c < count; ++c) {
    if (!visits[c].empty()) {
      heard.push_back(usual[c]);
    }
  }
  if (heard.empty()) {
    return {};
  }
  std::nth_element(heard.begin(), heard.begin() + static_cast<std::ptrdiff_t>(heard.size() / 2),
                   heard.end());
  const float floor = heard[heard.size() / 2];

  // Score each channel, then keep only the strongest within ±2 channels:
  // a strong signal also spills a little into its neighbours.
  std::vector<float> score(count, -1.0f);
  std::vector<bool> steady(count, false);
  for (std::size_t c = 0; c < count; ++c) {
    if (visits[c].empty()) {
      continue;
    }
    if (usual[c] - floor >= kSteadyDb) {
      steady[c] = true;
      score[c] = usual[c] - floor;
    } else if (loudest[c] - usual[c] >= kTalkPeakDb && above[c] >= kTalkMinTimes) {
      score[c] = loudest[c] - floor;
    }
  }
  std::vector<ScanHit> hits;
  for (std::size_t c = 0; c < count; ++c) {
    if (score[c] < 0.0f) {
      continue;
    }
    bool best = true;
    for (std::size_t d = (c >= 2 ? c - 2 : 0); d <= std::min(count - 1, c + 2); ++d) {
      if (d != c && (score[d] > score[c] || (score[d] == score[c] && d < c))) {
        best = false;
      }
    }
    if (!best) {
      continue;
    }
    ScanHit hit;
    hit.hz = band_channel_hz(band, c);
    hit.mode = band.mode;
    hit.steady = steady[c];
    hit.level_db = steady[c] ? usual[c] - floor : loudest[c] - usual[c];
    hit.times_heard = above[c];
    // Channels where two tuner windows overlap get more visits, so time
    // each channel by its own visit count.
    const double per_visit = seconds / static_cast<double>(visits[c].size());
    hit.last_heard_s =
        steady[c] ? 0.0f
                  : static_cast<float>(static_cast<double>(visits[c].size() - 1 - last_above[c]) *
                                       per_visit);
    hits.push_back(hit);
  }
  mark_interference(hits, band.step_hz);
  return hits;
}

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
  Fft fft(kFftSize);
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

int run_band_search(const BandSearchConfig &config, std::vector<ScanHit> *hits,
                    ScanProgress progress, void *user) {
  hits->clear();
  if (config.band == nullptr || config.lna_db > 40 || config.lna_db % 8 != 0 ||
      config.vga_db > 26 || config.vga_db % 2 != 0) {
    std::fprintf(stderr, "search: bad band or gain\n");
    return 1;
  }
  const Band &band = *config.band;
  const std::size_t count = band_channels(band);
  Fft fft(kWideFftSize);
  if (!fft.ok()) {
    return 1;
  }
  Session session;
  if (!session.ok()) {
    return 1;
  }
  hackrf_device *dev = session.get();
  // Tuner positions: each one covers kWideNearHz..kWideFarHz above itself.
  std::vector<std::uint64_t> los;
  const std::uint64_t first_lo = band.first_hz - static_cast<std::uint64_t>(kWideNearHz * 2);
  for (std::uint64_t lo = first_lo;; lo += static_cast<std::uint64_t>(kWideStepHz)) {
    los.push_back(lo);
    if (static_cast<double>(lo) + kWideFarHz >= static_cast<double>(band.last_hz)) {
      break;
    }
  }
  const std::uint32_t baseband = hackrf_compute_baseband_filter_bw(kWideBasebandHz);
  if (!rx_check("hackrf_set_sample_rate_manual",
                hackrf_set_sample_rate_manual(dev, kWideRateHz, 1)) ||
      !rx_check("hackrf_set_baseband_filter_bandwidth",
                hackrf_set_baseband_filter_bandwidth(dev, baseband)) ||
      !rx_check("hackrf_set_freq", hackrf_set_freq(dev, los[0])) ||
      !rx_check("hackrf_set_amp_enable", hackrf_set_amp_enable(dev, config.amp ? 1 : 0)) ||
      !rx_check("hackrf_set_lna_gain", hackrf_set_lna_gain(dev, config.lna_db)) ||
      !rx_check("hackrf_set_vga_gain", hackrf_set_vga_gain(dev, config.vga_db))) {
    return 1;
  }
  SpscRing<std::uint8_t> ring(1u << 23);
  IqSink sink;
  sink.ring = &ring;
  RxStream rx(dev);
  if (!rx.start(&sink)) {
    return 1;
  }
  std::vector<std::uint8_t> scratch(1u << 20);
  const std::size_t look_bytes = static_cast<std::size_t>(kWideFrames * kWideFftSize) * 2;
  std::vector<std::uint8_t> look(look_bytes);
  std::vector<std::vector<float>> visits(count);
  const double bin_hz = static_cast<double>(kWideRateHz) / kWideFftSize;
  const auto half_bins = std::max(1, static_cast<int>(band.half_width_hz / bin_hz));

  const auto start = std::chrono::steady_clock::now();
  int sweep = 0;
  std::size_t talking_so_far = 0;
  while (true) {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (elapsed >= config.seconds) {
      break;
    }
    // Every couple of seconds, count what is talking so far for the progress line.
    if (sweep % 20 == 0 && sweep > 0) {
      talking_so_far = 0;
      for (const ScanHit &hit : find_activity(band, visits, elapsed)) {
        talking_so_far += hit.interference ? 0 : 1;
      }
    }
    char what[96];
    std::snprintf(what, sizeof what, "watching %s · %.0f s · %zu in use so far", band.label,
                  elapsed, talking_so_far);
    if (!tell(progress, user, static_cast<float>(elapsed / config.seconds), what)) {
      break;
    }
    for (const std::uint64_t lo : los) {
      if (!rx_check("hackrf_set_freq", hackrf_set_freq(dev, lo))) {
        return 1;
      }
      drain(ring, scratch);
      if (!read_bytes(ring, scratch.data(), kWideSettleBytes) ||
          !read_bytes(ring, look.data(), look.size())) {
        std::fprintf(stderr, "search: the radio stopped sending samples\n");
        return 1;
      }
      if (elapsed < kWarmupS) {
        continue; // the first sweeps after starting are unsettled
      }
      const std::vector<double> &power = fft.average(look.data(), look.size());
      for (std::size_t c = 0; c < count; ++c) {
        const double offset =
            static_cast<double>(band_channel_hz(band, c)) - static_cast<double>(lo);
        if (offset < kWideNearHz || offset > kWideFarHz) {
          continue;
        }
        const int centre = kWideFftSize / 2 + static_cast<int>(std::lround(offset / bin_hz));
        double sum = 0.0;
        int bins = 0;
        for (int b = centre - half_bins; b <= centre + half_bins; ++b) {
          if (b >= 0 && b < kWideFftSize) {
            sum += power[static_cast<std::size_t>(b)];
            ++bins;
          }
        }
        if (bins > 0) {
          visits[c].push_back(static_cast<float>(10.0 * std::log10(sum / bins + 1e-30)));
        }
      }
    }
    ++sweep;
  }
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (sweep > 0) {
    *hits = find_activity(band, visits, elapsed);
  }
  tell(progress, user, 1.0f, "done");
  return 0;
}

} // namespace vocatron
