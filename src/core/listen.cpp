// Live receiver.
//
// Three threads, and each one has one job:
//   USB thread  — copy raw I/Q bytes into a queue and return.
//   DSP thread  — tune, demodulate, and push 48 kHz audio into a second queue.
//   Audio thread — copy that audio to the speakers. If the queue is empty,
//                  play silence.
// The main thread prints a status line, or hands each tick to the TUI.

#include "listen.hpp"

#include "channels.hpp"
#include "dsp.hpp"
#include "hackrf_session.hpp"
#include "playback.hpp"
#include "restricted.hpp"
#include "ring.hpp"
#include "rx.hpp"
#include "squelch.hpp"

#include <hackrf.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vocatron {
namespace {

constexpr std::uint32_t kSampleRateHz = 2'400'000;
constexpr std::uint32_t kBasebandRequestHz = 1'750'000;
constexpr std::uint64_t kLoOffsetHz = 250'000;
constexpr float kShiftHz = 250'000.0f;
// After a station change, drop this much I/Q (about 55 ms). It covers USB
// transfers that were already in flight from the old frequency and the
// tuner settling. The demodulator then mutes its own first 50 ms.
constexpr std::size_t kSettleBytes = 1u << 18;
// Audio cushion before the speaker plays: 150 ms at 48 kHz. Big enough to
// ride over one late USB lump, small enough that a station change feels quick.
constexpr std::size_t kPrimeFrames = 7200;
// If the radio's clock runs a little fast, audio would pile up and the delay
// would grow. Past half a second, new audio is dropped instead.
constexpr std::size_t kMaxQueuedFrames = 24000;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*sig*/) { g_stop = 1; }

struct Signals {
  struct sigaction old_int{};
  struct sigaction old_term{};
  bool installed = false;

  Signals() {
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int) == 0 && sigaction(SIGTERM, &action, &old_term) == 0) {
      installed = true;
    }
  }

  ~Signals() {
    if (installed) {
      sigaction(SIGINT, &old_int, nullptr);
      sigaction(SIGTERM, &old_term, nullptr);
    }
  }

  Signals(const Signals &) = delete;
  Signals &operator=(const Signals &) = delete;
};

bool check_call(const char *what, int rc) {
  if (rc == HACKRF_SUCCESS) {
    return true;
  }
  print_error(what, rc);
  return false;
}

int8_t as_int8(std::uint8_t byte) {
  return byte < 128 ? static_cast<int8_t>(byte) : static_cast<int8_t>(static_cast<int>(byte) - 256);
}

struct Levels {
  std::atomic<float> adc_dbfs{-120.0f};
  std::atomic<std::uint64_t> clip_samples{0};
  std::atomic<std::uint64_t> total_samples{0};
  std::atomic<float> raw_peak{0.0f};
  std::atomic<std::uint64_t> audio_dropped{0};
  std::atomic<float> channel_dbfs{-200.0f};
  std::atomic<bool> sql_open{false};
  // Wide FM only. The RDS name is 8 letters packed into one number.
  std::atomic<float> clarity{-1.0f};
  std::atomic<std::uint64_t> rds_name{0};
};

void meter_iq(const std::uint8_t *bytes, std::size_t n, Levels &levels) {
  if (n < 2) {
    return;
  }
  n -= n % 2;
  double sum_sq = 0.0;
  std::uint64_t clips = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const int sample = as_int8(bytes[i]);
    if (sample <= -127 || sample >= 127) {
      ++clips;
    }
    const double v = static_cast<double>(sample);
    sum_sq += v * v;
  }
  levels.clip_samples.fetch_add(clips, std::memory_order_relaxed);
  levels.total_samples.fetch_add(n, std::memory_order_relaxed);
  const double rms = std::sqrt(sum_sq / static_cast<double>(n)) / 128.0;
  float db = -120.0f;
  if (rms > 1e-12) {
    db = static_cast<float>(20.0 * std::log10(rms));
  }
  levels.adc_dbfs.store(db, std::memory_order_relaxed);
}

float bump_peak(std::atomic<float> &slot, float peak) {
  float prev = slot.load(std::memory_order_relaxed);
  while (peak > prev && !slot.compare_exchange_weak(prev, peak, std::memory_order_relaxed)) {
  }
  return peak;
}

// 8 letters packed one per byte (first in the low byte) into a trimmed,
// NUL-ended string.
void unpack_name(std::uint64_t packed, char out[9]) {
  int start = 0;
  int end = 0;
  char letters[8];
  for (int i = 0; i < 8; ++i) {
    letters[i] = static_cast<char>((packed >> (8 * i)) & 0xFFu);
  }
  while (start < 8 && (letters[start] == ' ' || letters[start] == '\0')) {
    ++start;
  }
  end = 8;
  while (end > start && (letters[end - 1] == ' ' || letters[end - 1] == '\0')) {
    --end;
  }
  int n = 0;
  for (int i = start; i < end; ++i) {
    out[n++] = letters[i];
  }
  out[n] = '\0';
}

enum class Demod { Wfm, Am, Nfm };

// "wfm", "am", or "nfm" to a Demod. False for anything else.
bool parse_demod(const char *mode, Demod *out) {
  if (mode == nullptr) {
    return false;
  }
  if (std::strcmp(mode, "wfm") == 0) {
    *out = Demod::Wfm;
  } else if (std::strcmp(mode, "am") == 0) {
    *out = Demod::Am;
  } else if (std::strcmp(mode, "nfm") == 0) {
    *out = Demod::Nfm;
  } else {
    return false;
  }
  return true;
}

const char *demod_tag(Demod demod) {
  switch (demod) {
  case Demod::Am:
    return "am";
  case Demod::Nfm:
    return "nfm";
  case Demod::Wfm:
    break;
  }
  return "wfm";
}

const char *demod_name(Demod demod) {
  switch (demod) {
  case Demod::Am:
    return "AM";
  case Demod::Nfm:
    return "narrow FM";
  case Demod::Wfm:
    break;
  }
  return "wide FM";
}

// Whichever one demodulator is playing. A station change throws the old
// one away and builds the new one, so no filter remembers the old station.
class Decoder {
public:
  bool build(Demod demod) {
    wfm_.reset();
    am_.reset();
    nfm_.reset();
    demod_ = demod;
    switch (demod) {
    case Demod::Am:
      return am_.emplace(kSampleRateHz, kShiftHz).ok();
    case Demod::Nfm:
      return nfm_.emplace(kSampleRateHz, kShiftHz).ok();
    case Demod::Wfm:
      break;
    }
    return wfm_.emplace(kSampleRateHz, kShiftHz).ok();
  }

  Demod demod() const { return demod_; }

  std::size_t process(const std::uint8_t *iq, std::size_t bytes, float *dst, std::size_t cap) {
    if (am_) {
      return am_->process(iq, bytes, dst, cap);
    }
    if (nfm_) {
      return nfm_->process(iq, bytes, dst, cap);
    }
    return wfm_ ? wfm_->process(iq, bytes, dst, cap) : 0;
  }

  float channel_dbfs() const {
    if (am_) {
      return am_->channel_dbfs();
    }
    if (nfm_) {
      return nfm_->channel_dbfs();
    }
    return wfm_ ? wfm_->channel_dbfs() : -200.0f;
  }

  float clarity() const { return wfm_ ? wfm_->clarity() : -1.0f; }
  std::uint64_t rds_name() const { return wfm_ ? wfm_->rds_name_packed() : 0; }

private:
  Demod demod_ = Demod::Wfm;
  std::optional<WfmStream> wfm_;
  std::optional<AmStream> am_;
  std::optional<NfmStream> nfm_;
};

} // namespace

int resolve_channel_name(const char *text, std::uint64_t *hz, const char **name, const char **mode,
                         const char **group) {
  const ChannelBook &book = channel_book();
  if (!book.error.empty()) {
    std::fprintf(stderr, "%s\n", book.error.c_str());
    return 1;
  }
  if (text == nullptr || hz == nullptr || name == nullptr) {
    return 2;
  }
  for (const Channel &channel : book.channels) {
    if (channel.name != text) {
      continue;
    }
    *hz = channel.hz;
    *name = channel.name.c_str();
    if (mode != nullptr) {
      *mode = channel.mode.c_str();
    }
    if (group != nullptr) {
      *group = channel.group.c_str();
    }
    return 0;
  }
  return 2;
}

int run_listen(const ListenConfig &config) {
  if (config.channel_hz <= kLoOffsetHz) {
    std::fprintf(stderr, "frequency is too low for the 250 kHz tune offset\n");
    return 1;
  }
  if (config.seconds != 0.0 && (config.seconds < 0.2 || config.seconds > 3600.0)) {
    std::fprintf(stderr,
                 "--seconds must be between 0.2 and 3600, or omitted to run until Ctrl-C\n");
    return 1;
  }
  if (config.out_path != nullptr && config.seconds == 0.0) {
    std::fprintf(stderr, "--out or --record needs --seconds, so the recording has an end\n");
    return 1;
  }
  {
    std::string why;
    if (!listen_allowed(config.channel_hz, config.permission_confirmed,
                        config.out_path != nullptr, &why)) {
      std::fprintf(stderr, "%s\n", why.c_str());
      return 1;
    }
  }
  if (config.group != nullptr && std::strcmp(config.group, "wireless") == 0 &&
      config.out_path != nullptr) {
    std::fprintf(stderr, "The wireless group is private. vocatron will not record it.\n");
    return 1;
  }
  if (config.lna_db > 40 || config.lna_db % 8 != 0) {
    std::fprintf(stderr, "--lna must be 0, 8, 16, 24, 32, or 40\n");
    return 1;
  }
  if (config.mode == nullptr) {
    std::fprintf(stderr, "mode must be wfm, am, or nfm\n");
    return 1;
  }
  Demod demod = Demod::Wfm;
  if (!parse_demod(config.mode, &demod)) {
    std::fprintf(stderr, "mode must be wfm, am, or nfm\n");
    return 1;
  }
  const char *mode_tag = demod_tag(demod);
  const char *mode_name = demod_name(demod);
  if (config.vga_db > 26 || config.vga_db % 2 != 0) {
    std::fprintf(stderr,
                 "--vga must be an even number from 0 to 26\n"
                 "VGA 28 and above can make this firmware accept the setting and then send no "
                 "samples.\n");
    return 1;
  }

  const char *name = config.name != nullptr ? config.name : "channel";
  g_stop = 0;
  if (config.signal_stop != nullptr) {
    *config.signal_stop = false;
  }
  const bool quiet = config.on_tick != nullptr;
  Signals signals;

  SpscRing<std::uint8_t> iq_ring(1u << 22);
  SpscRing<float> audio_ring(1u << 16);
  IqSink sink;
  sink.ring = &iq_ring;
  Levels levels;
  std::atomic<std::uint64_t> underruns{0};
  std::atomic<std::uint64_t> played{0};
  std::atomic<bool> producer_done{false};

  std::vector<float> recorded;
  if (config.out_path != nullptr) {
    const auto frames = static_cast<std::size_t>(config.seconds * 48000.0) + 48000;
    recorded.reserve(frames);
  }

  PlaybackUser playback;
  playback.ring = &audio_ring;
  playback.underruns = &underruns;
  playback.played = &played;
  playback.prime_frames = kPrimeFrames;
  Speakers speakers;
  const char *backend = "";
  if (config.speakers) {
    if (!speakers.open(&playback, "pulse", &backend)) {
      return 1;
    }
  }

  Session session;
  if (!session.ok()) {
    return 1;
  }
  if (!quiet && print_identity(session) != 0) {
    return 1;
  }

  const std::uint64_t lo_hz = config.channel_hz - kLoOffsetHz;
  const std::uint32_t baseband = hackrf_compute_baseband_filter_bw(kBasebandRequestHz);
  if (!quiet) {
    std::printf("listen: %s  %.3f MHz  %s\n", name, static_cast<double>(config.channel_hz) / 1.0e6,
                mode_name);
    std::printf("LO %.3f MHz (250 kHz below), %.3f MS/s, baseband %.3f MHz\n",
                static_cast<double>(lo_hz) / 1.0e6, kSampleRateHz / 1.0e6, baseband / 1.0e6);
    std::printf("amp %s, LNA %u dB, VGA %u dB, speakers %s%s%s\n", config.amp ? "ON" : "off",
                config.lna_db, config.vga_db, config.speakers ? "on" : "off",
                config.speakers ? " via " : "", config.speakers ? backend : "");
    std::printf("A shut squelch is silence. A thin voice needs more LNA.\n");
    if (config.amp) {
      std::printf("RF amp is ON. A strong station can overload the receiver.\n");
    }
    if (config.squelch) {
      std::printf("squelch opens above %.0f dBFS and holds 0.3 s after the channel drops\n",
                  static_cast<double>(config.squelch_dbfs));
    }
    if (config.seconds == 0.0) {
      std::printf("Ctrl-C to stop.\n");
    }
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

  // A station change from the tick. The main thread moves the tuner, then
  // bumps tune_gen. The DSP thread sees the new number and swaps demodulators.
  std::atomic<int> want_demod{static_cast<int>(demod)};
  std::atomic<unsigned> tune_gen{0};
  std::atomic<bool> dsp_failed{false};

  std::thread dsp([&] {
    Decoder decoder;
    if (!decoder.build(demod)) {
      std::fprintf(stderr, "%s demodulator did not start\n", mode_name);
      dsp_failed.store(true, std::memory_order_relaxed);
      producer_done.store(true, std::memory_order_release);
      return;
    }
    std::vector<std::uint8_t> chunk(1u << 18);
    std::vector<float> audio(8192);
    std::vector<float> gained(8192);
    Squelch squelch(config.squelch, config.squelch_dbfs, 48000);
    float play_gain = play_gain_for_mode(demod_tag(demod));
    unsigned seen_gen = 0;
    std::size_t settle_bytes = 0;
    while (true) {
      const unsigned gen = tune_gen.load(std::memory_order_acquire);
      if (gen != seen_gen) {
        // New station. This builds new filters, so it allocates once here,
        // while the audio is muted. Steady listening still never allocates.
        seen_gen = gen;
        const auto next = static_cast<Demod>(want_demod.load(std::memory_order_relaxed));
        if (!decoder.build(next)) {
          std::fprintf(stderr, "%s demodulator did not start\n", demod_name(next));
          dsp_failed.store(true, std::memory_order_relaxed);
          break;
        }
        play_gain = play_gain_for_mode(demod_tag(next));
        // Everything already queued was tuned to the old station.
        while (iq_ring.try_read(chunk.data(), chunk.size()) > 0) {
        }
        settle_bytes = kSettleBytes;
        // Start the new station with the gate shut and fresh meters.
        squelch = Squelch(config.squelch, config.squelch_dbfs, 48000);
        levels.clip_samples.store(0, std::memory_order_relaxed);
        levels.total_samples.store(0, std::memory_order_relaxed);
        levels.raw_peak.store(0.0f, std::memory_order_relaxed);
        levels.channel_dbfs.store(-200.0f, std::memory_order_relaxed);
        levels.clarity.store(-1.0f, std::memory_order_relaxed);
        levels.rds_name.store(0, std::memory_order_relaxed);
      }
      std::size_t n = iq_ring.try_read(chunk.data(), chunk.size());
      if (n == 0) {
        if (producer_done.load(std::memory_order_acquire)) {
          n = iq_ring.try_read(chunk.data(), chunk.size());
          if (n == 0) {
            break;
          }
        } else {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
          continue;
        }
      }
      if (settle_bytes > 0) {
        // USB was still carrying samples from before the tuner moved.
        const std::size_t drop = std::min(n, settle_bytes);
        settle_bytes -= drop;
        n -= drop;
        if (n == 0) {
          continue;
        }
        std::memmove(chunk.data(), chunk.data() + drop, n);
      }
      meter_iq(chunk.data(), n, levels);
      const std::size_t got = decoder.process(chunk.data(), n, audio.data(), audio.size());
      const float channel_db = decoder.channel_dbfs();
      levels.clarity.store(decoder.clarity(), std::memory_order_relaxed);
      levels.rds_name.store(decoder.rds_name(), std::memory_order_relaxed);
      if (got > 0) {
        if (config.squelch_enabled != nullptr && config.squelch_level != nullptr) {
          squelch.set_level(config.squelch_enabled->load(std::memory_order_relaxed),
                            config.squelch_level->load(std::memory_order_relaxed));
        }
        levels.channel_dbfs.store(channel_db, std::memory_order_relaxed);
        const bool open = squelch.apply(audio.data(), got, channel_db);
        levels.sql_open.store(open, std::memory_order_relaxed);
      }
      float chunk_peak = 0.0f;
      for (std::size_t i = 0; i < got; ++i) {
        chunk_peak = std::max(chunk_peak, std::fabs(audio[i]));
        float sample = audio[i] * play_gain;
        if (sample > 1.0f) {
          sample = 1.0f;
        } else if (sample < -1.0f) {
          sample = -1.0f;
        }
        gained[i] = sample;
      }
      if (got > 0) {
        bump_peak(levels.raw_peak, chunk_peak);
      }
      if (got > 0 && config.pass_audio != nullptr &&
          !config.pass_audio->load(std::memory_order_relaxed)) {
        std::fill_n(gained.begin(), got, 0.0f);
      }
      if (config.out_path != nullptr && got > 0) {
        const std::size_t room = recorded.capacity() - recorded.size();
        const std::size_t take = got < room ? got : room;
        recorded.insert(recorded.end(), gained.begin(),
                        gained.begin() + static_cast<std::ptrdiff_t>(take));
      }
      if (config.speakers && got > 0 && audio_ring.size_approx() > kMaxQueuedFrames) {
        levels.audio_dropped.fetch_add(got, std::memory_order_relaxed);
      } else if (config.speakers && got > 0) {
        const std::size_t wrote = audio_ring.try_write(gained.data(), got);
        if (wrote < got) {
          levels.audio_dropped.fetch_add(got - wrote, std::memory_order_relaxed);
        }
      }
    }
  });

  RxStream rx(dev);
  if (!rx.start(&sink)) {
    producer_done.store(true, std::memory_order_release);
    dsp.join();
    return 1;
  }

  const auto t0 = std::chrono::steady_clock::now();
  bool stalled = false;
  bool speakers_on = false;
  bool stop_now = false;
  auto publish = [&](double elapsed) -> bool {
    if (!quiet) {
      return true;
    }
    ListenView view;
    const std::uint64_t total = levels.total_samples.load(std::memory_order_relaxed);
    const std::uint64_t clips = levels.clip_samples.load(std::memory_order_relaxed);
    view.channel_dbfs = levels.channel_dbfs.load(std::memory_order_relaxed);
    view.adc_dbfs = levels.adc_dbfs.load(std::memory_order_relaxed);
    view.clip_pct =
        total == 0
            ? 0.0f
            : static_cast<float>(100.0 * static_cast<double>(clips) / static_cast<double>(total));
    view.sql_open = levels.sql_open.load(std::memory_order_relaxed);
    view.dropped = sink.dropped.load(std::memory_order_relaxed);
    view.underruns = underruns.load(std::memory_order_relaxed);
    view.demod_peak = levels.raw_peak.load(std::memory_order_relaxed);
    view.elapsed_s = elapsed;
    view.usb_bytes = sink.usb_bytes.load(std::memory_order_relaxed);
    view.clarity = levels.clarity.load(std::memory_order_relaxed);
    unpack_name(levels.rds_name.load(std::memory_order_relaxed), view.rds_name);
    return config.on_tick(view, config.tick_user);
  };
  if (config.speakers) {
    const auto prefill_deadline = t0 + std::chrono::milliseconds(500);
    auto next_publish = t0;
    while (audio_ring.size_approx() < kPrimeFrames &&
           std::chrono::steady_clock::now() < prefill_deadline && g_stop == 0 && !stop_now) {
      const auto pre_now = std::chrono::steady_clock::now();
      if (pre_now >= next_publish) {
        next_publish = pre_now + std::chrono::milliseconds(50);
        const double elapsed = std::chrono::duration<double>(pre_now - t0).count();
        if (!publish(elapsed)) {
          stop_now = true;
          break;
        }
      }
      if (sink.usb_bytes.load(std::memory_order_relaxed) == 0 &&
          std::chrono::steady_clock::now() - t0 > std::chrono::seconds(1)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (g_stop == 0 && !stop_now) {
      speakers_on = speakers.start();
      if (!speakers_on) {
        g_stop = 1;
      }
    }
  }

  // Move the tuner and LNA while the stream keeps running, then tell the
  // DSP thread to swap demodulators. The radio and speakers stay open.
  bool failed = false;
  // Station and gains in effect right now, so a gain-only change can skip
  // rebuilding the demodulator.
  std::uint64_t tuned_hz = config.channel_hz;
  Demod tuned_demod = demod;
  std::uint32_t tuned_lna = config.lna_db;
  std::uint32_t tuned_vga = config.vga_db;
  bool tuned_amp = config.amp;
  auto apply_retune = [&](ListenRetune &retune) -> bool {
    retune.pending = false;
    Demod next = Demod::Wfm;
    if (!parse_demod(retune.mode, &next) || retune.channel_hz <= kLoOffsetHz ||
        retune.lna_db > 40 || retune.lna_db % 8 != 0 || retune.vga_db > 26 ||
        retune.vga_db % 2 != 0) {
      return false;
    }
    const bool new_station = retune.channel_hz != tuned_hz || next != tuned_demod;
    // A recording is one station from start to end.
    if (new_station && config.out_path != nullptr) {
      return false;
    }
    // Restricted frequencies need confirmed permission, here as at start-up.
    if (new_station && !listen_allowed(retune.channel_hz, retune.permission_confirmed,
                                       config.out_path != nullptr, nullptr)) {
      return false;
    }
    if (new_station &&
        !check_call("hackrf_set_freq", hackrf_set_freq(dev, retune.channel_hz - kLoOffsetHz))) {
      return false;
    }
    if (retune.lna_db != tuned_lna &&
        !check_call("hackrf_set_lna_gain", hackrf_set_lna_gain(dev, retune.lna_db))) {
      return false;
    }
    if (retune.vga_db != tuned_vga &&
        !check_call("hackrf_set_vga_gain", hackrf_set_vga_gain(dev, retune.vga_db))) {
      return false;
    }
    // The amp only turns on here when the caller asked for it explicitly.
    if (retune.amp != tuned_amp &&
        !check_call("hackrf_set_amp_enable", hackrf_set_amp_enable(dev, retune.amp ? 1 : 0))) {
      return false;
    }
    tuned_hz = retune.channel_hz;
    tuned_demod = next;
    tuned_lna = retune.lna_db;
    tuned_vga = retune.vga_db;
    tuned_amp = retune.amp;
    if (new_station) {
      want_demod.store(static_cast<int>(next), std::memory_order_relaxed);
      tune_gen.fetch_add(1, std::memory_order_release);
    } else {
      // Same station, new gain. Clip % should describe the new setting.
      levels.clip_samples.store(0, std::memory_order_relaxed);
      levels.total_samples.store(0, std::memory_order_relaxed);
    }
    return true;
  };

  auto next_status = t0 + std::chrono::seconds(1);
  const bool tty = isatty(STDOUT_FILENO) == 1;
  while (g_stop == 0 && !stop_now) {
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - t0).count();
    if (config.seconds > 0.0 && elapsed >= config.seconds) {
      break;
    }
    if (sink.usb_bytes.load(std::memory_order_relaxed) == 0 && now - t0 > std::chrono::seconds(1)) {
      stalled = true;
      if (!quiet) {
        std::fprintf(stderr, "\nStreaming produced no samples.\n");
      }
      break;
    }
    const int streaming = hackrf_is_streaming(dev);
    if (streaming != HACKRF_TRUE && now - t0 > std::chrono::milliseconds(500)) {
      stalled = sink.usb_bytes.load(std::memory_order_relaxed) == 0;
      if (streaming != HACKRF_ERROR_STREAMING_EXIT_CALLED) {
        std::fprintf(stderr, "\n");
        print_error("hackrf_is_streaming", streaming);
      }
      break;
    }
    if (dsp_failed.load(std::memory_order_relaxed)) {
      failed = true;
      break;
    }
    if (quiet) {
      if (!publish(elapsed)) {
        stop_now = true;
        break;
      }
      if (config.retune != nullptr && config.retune->pending && !apply_retune(*config.retune)) {
        failed = true;
        break;
      }
    } else if (now >= next_status) {
      next_status = now + std::chrono::seconds(1);
      const std::uint64_t total = levels.total_samples.load(std::memory_order_relaxed);
      if (total > 0) {
        const std::uint64_t clips = levels.clip_samples.load(std::memory_order_relaxed);
        const double clip_pct = 100.0 * static_cast<double>(clips) / static_cast<double>(total);
        const char *sql = "off";
        if (config.squelch) {
          sql = levels.sql_open.load(std::memory_order_relaxed) ? "open" : "shut";
        }
        std::printf("%s%-14s %-3s  %7.3f MHz  ch %6.1f dBFS  adc %6.1f  clip %5.2f%%  "
                    "sql %-4s  dropped %llu  underruns %llu   ",
                    tty ? "\r" : "", name, mode_tag, static_cast<double>(config.channel_hz) / 1.0e6,
                    static_cast<double>(levels.channel_dbfs.load(std::memory_order_relaxed)),
                    static_cast<double>(levels.adc_dbfs.load(std::memory_order_relaxed)), clip_pct,
                    sql,
                    static_cast<unsigned long long>(sink.dropped.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(underruns.load(std::memory_order_relaxed)));
        if (!tty) {
          std::printf("\n");
        }
        std::fflush(stdout);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  rx.stop();
  producer_done.store(true, std::memory_order_release);
  dsp.join();
  speakers.stop();

  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const std::uint64_t dropped = sink.dropped.load(std::memory_order_relaxed);
  const std::uint64_t usb_bytes = sink.usb_bytes.load(std::memory_order_relaxed);
  if (config.signal_stop != nullptr) {
    *config.signal_stop = g_stop != 0;
  }
  if (!quiet) {
    const char *sql = "off";
    if (config.squelch) {
      sql = levels.sql_open.load(std::memory_order_relaxed) ? "open" : "shut";
    }
    std::printf("\nlistened %.2f s, channel %.1f dBFS, sql %s, %llu USB bytes, dropped %llu, "
                "audio underruns %llu, demod peak %.3f\n",
                elapsed, static_cast<double>(levels.channel_dbfs.load(std::memory_order_relaxed)),
                sql, static_cast<unsigned long long>(usb_bytes),
                static_cast<unsigned long long>(dropped),
                static_cast<unsigned long long>(underruns.load(std::memory_order_relaxed)),
                static_cast<double>(levels.raw_peak.load(std::memory_order_relaxed)));
  }

  if (config.out_path != nullptr && !recorded.empty()) {
    std::filesystem::path out(config.out_path);
    std::error_code ec;
    if (out.has_parent_path()) {
      std::filesystem::create_directories(out.parent_path(), ec);
    }
    if (ec || !write_wav_s16(config.out_path, recorded.data(), recorded.size(), 48000, 0.0f)) {
      std::fprintf(stderr, "could not write %s\n", config.out_path);
      return 1;
    }
    std::printf("wrote %s (%.2f s)\n", config.out_path,
                static_cast<double>(recorded.size()) / 48000.0);
  }

  if (failed) {
    return 1;
  }
  if (!stop_now && (stalled || usb_bytes == 0)) {
    return 1;
  }
  // The screen chose to stop. A few dropped bytes earlier is not a reason
  // to report the receiver as broken.
  if (dropped != 0 && !stop_now) {
    std::fprintf(stderr, "The sample queue dropped bytes. The demodulator did not keep up.\n");
    return 1;
  }
  if (config.speakers && !speakers_on && !stop_now) {
    return 1;
  }
  if (config.speakers && played.load(std::memory_order_relaxed) == 0 && elapsed > 0.5) {
    std::fprintf(stderr, "The speakers were open but no audio was played.\n");
    return 1;
  }
  return 0;
}

} // namespace vocatron
