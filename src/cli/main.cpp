#include "core/capture.hpp"
#include "core/channels.hpp"
#include "core/dsp.hpp"
#include "core/hackrf_session.hpp"
#include "core/listen.hpp"
#include "core/scan.hpp"
#include "tui.hpp"

#include <hackrf.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

void print_usage() {
  std::fprintf(
      stderr,
      "vocatron info\n"
      "    Print the connected HackRF.\n"
      "vocatron capture <MHz> [--seconds 5] [-o samples/fm.iq] [--lna 16] [--vga 20] [--amp]\n"
      "    Record receive-only I/Q and print the strongest frequencies.\n"
      "vocatron selftest\n"
      "    Check the spectrum math on a fake tone. Does not open the radio.\n"
      "vocatron ringtest [--seconds 3] [--lna 16] [--vga 20]\n"
      "    Stream 2.4 MS/s through the sample queue and count dropped bytes.\n"
      "vocatron scan [--lna 40] [--vga 26] [--amp]\n"
      "    Find the FM stations this radio can hear, with clarity and RDS names.\n"
      "vocatron list [--group airband]\n"
      "    Print the stations in channels/istanbul.toml.\n"
      "vocatron tui\n"
      "    Open the station screen and play the highlighted channel.\n"
      "    Squelch starts on, so an empty channel stays quiet.\n"
      "vocatron demod <file.iq> --mode wfm|am|nfm --out <file.wav> [--shift-khz 250]\n"
      "    Demodulate a capture to a 48 kHz WAV. No radio.\n"
      "vocatron listen POWER_FM [--seconds N] [--lna 16] [--vga 20] [--amp] [--out a.wav]\n"
      "    Play wide FM. LTBA_ATIS is AM. CH16, RU1, and PMR1 are narrow FM.\n"
      "    Ctrl-C stops. --no-speakers skips the sound device.\n"
      "    --out and --record write a WAV, and both need --seconds.\n"
      "    --squelch -40 mutes while the channel is quieter than -40 dBFS.\n"
      "    Empty narrow FM sat near -55, so -40 stays shut. Wide FM at\n"
      "    LNA 40 sat near -34, so use about -20 there.\n"
      "    A frequency works too: vocatron listen 156.8 --mode nfm\n");
}

bool parse_mhz(const char *text, uint64_t *hz) {
  char *end = nullptr;
  const double mhz = std::strtod(text, &end);
  if (end == text || *end != '\0' || mhz < 1.0 || mhz > 6000.0) {
    return false;
  }
  *hz = static_cast<uint64_t>(std::llround(mhz * 1'000'000.0));
  return true;
}

bool parse_u32(const char *text, uint32_t *out) {
  char *end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value > 1000UL) {
    return false;
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

bool parse_db(const char *text, float *out) {
  char *end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0' || value < -120.0 || value > 0.0) {
    return false;
  }
  *out = static_cast<float>(value);
  return true;
}

bool parse_seconds(const char *text, double *out) {
  char *end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

int run_demod_cli(int argc, char **argv) {
  const char *in_path = nullptr;
  const char *out_path = nullptr;
  const char *mode = nullptr;
  double shift_khz = 250.0;
  for (int i = 2; i < argc; ++i) {
    const char *arg = argv[i];
    if (std::strcmp(arg, "--mode") == 0 || std::strcmp(arg, "--out") == 0 ||
        std::strcmp(arg, "--shift-khz") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", arg);
        return 2;
      }
      const char *value = argv[++i];
      if (std::strcmp(arg, "--mode") == 0) {
        mode = value;
      } else if (std::strcmp(arg, "--out") == 0) {
        out_path = value;
      } else if (!parse_seconds(value, &shift_khz)) {
        std::fprintf(stderr, "--shift-khz needs a number\n");
        return 2;
      }
    } else if (arg[0] != '-' && in_path == nullptr) {
      in_path = arg;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg);
      print_usage();
      return 2;
    }
  }
  if (in_path == nullptr || out_path == nullptr || mode == nullptr) {
    std::fprintf(stderr, "demod needs a file, --mode wfm|am|nfm, and --out a.wav\n");
    return 2;
  }
  if (std::strcmp(mode, "wfm") != 0 && std::strcmp(mode, "am") != 0 &&
      std::strcmp(mode, "nfm") != 0) {
    std::fprintf(stderr, "only --mode wfm, am, and nfm are implemented\n");
    return 2;
  }

  FILE *file = std::fopen(in_path, "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "could not open %s\n", in_path);
    return 1;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    std::fprintf(stderr, "could not read %s\n", in_path);
    return 1;
  }
  const long length = std::ftell(file);
  if (length <= 0) {
    std::fclose(file);
    std::fprintf(stderr, "%s is empty\n", in_path);
    return 1;
  }
  std::rewind(file);
  std::vector<std::uint8_t> iq(static_cast<std::size_t>(length));
  const std::size_t got = std::fread(iq.data(), 1, iq.size(), file);
  std::fclose(file);
  if (got != iq.size()) {
    std::fprintf(stderr, "short read of %s\n", in_path);
    return 1;
  }

  const float shift_hz = static_cast<float>(shift_khz * 1000.0);
  vocatron::WfmResult audio;
  if (std::strcmp(mode, "am") == 0) {
    audio = vocatron::demod_am(iq.data(), iq.size(), 2'400'000, shift_hz);
  } else if (std::strcmp(mode, "nfm") == 0) {
    audio = vocatron::demod_nfm(iq.data(), iq.size(), 2'400'000, shift_hz);
  } else {
    audio = vocatron::demod_wfm(iq.data(), iq.size(), 2'400'000, shift_hz);
  }
  if (!audio.ok) {
    std::fprintf(stderr, "demod failed. The capture must be 2.4 MS/s int8 I/Q and long enough.\n");
    return 1;
  }
  if (!vocatron::write_wav_s16(out_path, audio.audio.data(), audio.audio.size(), 48000)) {
    std::fprintf(stderr, "could not write %s\n", out_path);
    return 1;
  }
  const double seconds = static_cast<double>(audio.audio.size()) / 48000.0;
  std::printf("wrote %s (%zu samples, %.2f s at 48 kHz, channel %.1f dBFS)\n", out_path,
              audio.audio.size(), seconds, static_cast<double>(audio.channel_dbfs));
  if (std::strcmp(mode, "wfm") == 0) {
    std::printf("clarity %.2f (0 buried, 1 clean)   RDS %s", static_cast<double>(audio.clarity),
                audio.rds_name.empty() ? "none" : audio.rds_name.c_str());
    if (audio.rds_pi != 0) {
      std::printf("  PI %04X", static_cast<unsigned>(audio.rds_pi));
    }
    std::printf("\n");
  }
  return 0;
}

int run_info() {
  vocatron::Session session;
  if (!session.ok()) {
    return 1;
  }
  return vocatron::print_identity(session);
}

int run_capture_cli(int argc, char **argv) {
  vocatron::CaptureConfig config;
  bool have_freq = false;
  for (int i = 2; i < argc; ++i) {
    const char *arg = argv[i];
    auto need = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", flag);
        return nullptr;
      }
      ++i;
      return argv[i];
    };

    if (std::strcmp(arg, "--seconds") == 0) {
      const char *value = need(arg);
      if (value == nullptr || !parse_seconds(value, &config.seconds)) {
        std::fprintf(stderr, "--seconds needs a number\n");
        return 2;
      }
    } else if (std::strcmp(arg, "-o") == 0 || std::strcmp(arg, "--out") == 0) {
      const char *value = need(arg);
      if (value == nullptr) {
        return 2;
      }
      config.out_path = value;
    } else if (std::strcmp(arg, "--lna") == 0) {
      const char *value = need(arg);
      if (value == nullptr || !parse_u32(value, &config.lna_db)) {
        std::fprintf(stderr, "--lna needs a number\n");
        return 2;
      }
    } else if (std::strcmp(arg, "--vga") == 0) {
      const char *value = need(arg);
      if (value == nullptr || !parse_u32(value, &config.vga_db)) {
        std::fprintf(stderr, "--vga needs a number\n");
        return 2;
      }
    } else if (std::strcmp(arg, "--amp") == 0) {
      config.amp = true;
    } else if (arg[0] != '-' && !have_freq) {
      if (!parse_mhz(arg, &config.channel_hz)) {
        std::fprintf(stderr, "frequency must be in MHz, from 1 to 6000 (example: 100.0)\n");
        return 2;
      }
      have_freq = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg);
      print_usage();
      return 2;
    }
  }

  if (!have_freq) {
    std::fprintf(stderr, "capture needs a frequency in MHz, for example: vocatron capture 100.0\n");
    return 2;
  }
  return vocatron::run_capture(config);
}

} // namespace

int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  const char *release = hackrf_library_release();
  const char *version = hackrf_library_version();
  std::printf("libhackrf %s (%s)\n", release != nullptr ? release : "?",
              version != nullptr ? version : "?");

  if (argc < 2 || std::strcmp(argv[1], "info") == 0) {
    return run_info();
  }
  if (std::strcmp(argv[1], "capture") == 0) {
    return run_capture_cli(argc, argv);
  }
  if (std::strcmp(argv[1], "demod") == 0) {
    return run_demod_cli(argc, argv);
  }
  if (std::strcmp(argv[1], "list") == 0) {
    const char *group = nullptr;
    for (int i = 2; i < argc; ++i) {
      const char *arg = argv[i];
      if (std::strcmp(arg, "--group") == 0) {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "--group needs a name, for example airband\n");
          return 2;
        }
        group = argv[++i];
      } else {
        std::fprintf(stderr, "unknown argument: %s\n", arg);
        print_usage();
        return 2;
      }
    }
    return vocatron::print_channel_list(group);
  }
  if (std::strcmp(argv[1], "tui") == 0) {
    if (argc != 2) {
      std::fprintf(stderr, "vocatron tui takes no arguments\n");
      return 2;
    }
    return vocatron::run_tui();
  }
  if (std::strcmp(argv[1], "listen") == 0) {
    vocatron::ListenConfig config;
    const char *target = nullptr;
    const char *mode = nullptr;
    for (int i = 2; i < argc; ++i) {
      const char *arg = argv[i];
      auto need = [&](const char *flag) -> const char * {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "%s needs a value\n", flag);
          return nullptr;
        }
        ++i;
        return argv[i];
      };
      if (std::strcmp(arg, "--seconds") == 0) {
        const char *value = need(arg);
        if (value == nullptr || !parse_seconds(value, &config.seconds)) {
          std::fprintf(stderr, "--seconds needs a number\n");
          return 2;
        }
      } else if (std::strcmp(arg, "--lna") == 0) {
        const char *value = need(arg);
        if (value == nullptr || !parse_u32(value, &config.lna_db)) {
          std::fprintf(stderr, "--lna needs a number\n");
          return 2;
        }
      } else if (std::strcmp(arg, "--vga") == 0) {
        const char *value = need(arg);
        if (value == nullptr || !parse_u32(value, &config.vga_db)) {
          std::fprintf(stderr, "--vga needs a number\n");
          return 2;
        }
      } else if (std::strcmp(arg, "--out") == 0 || std::strcmp(arg, "--record") == 0) {
        const char *value = need(arg);
        if (value == nullptr) {
          return 2;
        }
        config.out_path = value;
      } else if (std::strcmp(arg, "--mode") == 0) {
        const char *value = need(arg);
        if (value == nullptr) {
          return 2;
        }
        mode = value;
      } else if (std::strcmp(arg, "--amp") == 0) {
        config.amp = true;
      } else if (std::strcmp(arg, "--squelch") == 0) {
        const char *value = need(arg);
        if (value == nullptr || !parse_db(value, &config.squelch_dbfs)) {
          std::fprintf(stderr, "--squelch needs a level from -120 to 0, for example -40\n");
          return 2;
        }
        config.squelch = true;
      } else if (std::strcmp(arg, "--no-speakers") == 0) {
        config.speakers = false;
      } else if (arg[0] != '-' && target == nullptr) {
        target = arg;
      } else {
        std::fprintf(stderr, "unknown argument: %s\n", arg);
        print_usage();
        return 2;
      }
    }
    if (target == nullptr) {
      std::fprintf(stderr, "listen needs a channel name or a frequency in MHz\n");
      return 2;
    }
    if (mode != nullptr && std::strcmp(mode, "wfm") != 0 && std::strcmp(mode, "am") != 0 &&
        std::strcmp(mode, "nfm") != 0) {
      std::fprintf(stderr, "only --mode wfm, am, and nfm are implemented\n");
      return 2;
    }
    const char *named_mode = nullptr;
    const char *named_group = nullptr;
    const int named = vocatron::resolve_channel_name(target, &config.channel_hz, &config.name,
                                                     &named_mode, &named_group);
    if (named == 1) {
      return 2;
    }
    if (named == 2) {
      if (!parse_mhz(target, &config.channel_hz)) {
        std::fprintf(stderr, "unknown channel \"%s\". Try POWER_FM, or a frequency like 100.0\n",
                     target);
        return 2;
      }
      config.name = target;
      config.mode = mode != nullptr ? mode : "wfm";
    } else {
      if (mode != nullptr && std::strcmp(mode, named_mode) != 0) {
        std::fprintf(stderr, "%s is %s, not %s\n", target, named_mode, mode);
        return 2;
      }
      config.mode = named_mode;
      config.group = named_group;
    }
    return vocatron::run_listen(config);
  }
  if (std::strcmp(argv[1], "selftest") == 0) {
    return vocatron::run_selftest();
  }
  if (std::strcmp(argv[1], "scan") == 0) {
    vocatron::ScanConfig config;
    for (int i = 2; i < argc; ++i) {
      const char *arg = argv[i];
      if (std::strcmp(arg, "--amp") == 0) {
        config.amp = true;
        continue;
      }
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", arg);
        return 2;
      }
      const char *value = argv[++i];
      std::uint32_t *slot = std::strcmp(arg, "--lna") == 0   ? &config.lna_db
                            : std::strcmp(arg, "--vga") == 0 ? &config.vga_db
                                                             : nullptr;
      if (slot == nullptr) {
        std::fprintf(stderr, "unknown argument: %s\n", arg);
        print_usage();
        return 2;
      }
      if (!parse_u32(value, slot)) {
        std::fprintf(stderr, "%s needs a number\n", arg);
        return 2;
      }
    }
    std::vector<vocatron::ScanHit> hits;
    const auto show = [](float fraction, const char *what, void *) {
      std::fprintf(stderr, "\r%3.0f%%  %-40s", static_cast<double>(fraction) * 100.0, what);
      return true;
    };
    const int rc = vocatron::run_fm_scan(config, &hits, show, nullptr);
    std::fprintf(stderr, "\n");
    if (rc != 0) {
      return rc;
    }
    std::printf("%zu stations\n   MHz    level  clarity  RDS\n", hits.size());
    for (const vocatron::ScanHit &hit : hits) {
      std::printf("%6.1f  %5.1f dB    %.2f   %s", static_cast<double>(hit.hz) / 1e6,
                  static_cast<double>(hit.level_db), static_cast<double>(hit.clarity),
                  hit.rds_name.c_str());
      if (hit.rds_pi != 0) {
        std::printf("  (PI %04X)", static_cast<unsigned>(hit.rds_pi));
      }
      std::printf("\n");
    }
    return 0;
  }
  if (std::strcmp(argv[1], "ringtest") == 0) {
    double seconds = 3.0;
    uint32_t lna = 16;
    uint32_t vga = 20;
    for (int i = 2; i < argc; ++i) {
      const char *arg = argv[i];
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", arg);
        return 2;
      }
      const char *value = argv[++i];
      if (std::strcmp(arg, "--seconds") == 0) {
        if (!parse_seconds(value, &seconds)) {
          std::fprintf(stderr, "--seconds needs a number\n");
          return 2;
        }
      } else if (std::strcmp(arg, "--lna") == 0) {
        if (!parse_u32(value, &lna)) {
          std::fprintf(stderr, "--lna needs a number\n");
          return 2;
        }
      } else if (std::strcmp(arg, "--vga") == 0) {
        if (!parse_u32(value, &vga)) {
          std::fprintf(stderr, "--vga needs a number\n");
          return 2;
        }
      } else {
        std::fprintf(stderr, "unknown argument: %s\n", arg);
        print_usage();
        return 2;
      }
    }
    return vocatron::run_ringtest(seconds, lna, vga);
  }
  if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
    print_usage();
    return 0;
  }
  std::fprintf(stderr, "unknown command: %s\n", argv[1]);
  print_usage();
  return 2;
}
