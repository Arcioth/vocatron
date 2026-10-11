# AGENTS.md — vocatron

Guidance for AI coding agents (and humans) working in this repo.
**The owner is a beginner in SDR/DSP and in C++.** Explain radio,
signal-processing and C++ concepts in plain words when you introduce them,
keep changes small, and make every step buildable and checkable on its own.

**Goal: maximum performance.** Independent project (not part of Raphael),
written in **C++20**, **headless CLI first**; a frontend comes later.

---

## 1. What this project is

`vocatron` is a small **receive-only** program that turns a HackRF (PortaPack
in "HackRF mode", connected over USB) into a voice/audio receiver on a Linux PC.

You pick a channel by name or frequency, the program:

1. pulls raw I/Q samples from the HackRF,
2. digitally tunes to the channel, filters it, and demodulates it (AM / NFM / WFM),
3. plays the audio on the speakers and optionally records it to WAV.

Stretch goals (only after the basics work): scanning a channel group,
listening to several channels in the same band at once, FM stereo/RDS.

### Hard rules

- **Receive only. Never write code that transmits** (no `hackrf_transfer -t`,
  no `hackrf_start_tx`, no `hackrf_set_antenna_enable` / bias tee). The only
  libhackrf streaming call allowed is `hackrf_start_rx`. Transmitting without a
  licence is illegal, and on aviation/marine frequencies it is dangerous.
- **Never enable the HackRF RF amp by default.** Max safe input is about
  −5 dBm; Istanbul has very strong FM/TV transmitters (Çamlıca). Amp off,
  moderate LNA gain is the default. The amp is an explicit opt-in flag.
- Ask before installing system packages or editing NixOS config.
- Do not add a dependency without saying why it's needed.

---

## 2. Hardware setup

| Item | Detail |
|---|---|
| Radio | HackRF One + PortaPack (Mayhem firmware) |
| Mode | On the PortaPack main menu choose **HackRF** → it now behaves like a plain HackRF One over USB |
| Coverage | 1 MHz – 6 GHz (performance is weaker below ~10 MHz) |
| ADC | 8-bit I/Q, 2–20 MS/s |
| Antenna | 5-stage telescopic whip |
| Host | NixOS, gcc available; everything else comes from the Nix flake |

Sanity check before writing any code: `hackrf_info` must list the device.
On NixOS this needs `hardware.hackrf.enable = true;` in the system config and
the user in the `plugdev` group (then log out/in).

### Known quirks (learned in the owner's earlier Raphael project)

- The unit seen so far is a **HackRF One r9 clone**, firmware `n_260928`.
- **Kernel driver grabs the device:** Linux's v4l `hackrf` module claims the
  USB interface and libhackrf then fails with *resource busy*. Fix:
  `sudo modprobe -r hackrf` (or blacklist it in the NixOS config). Print this
  hint when `hackrf_open` returns `HACKRF_ERROR_LIBUSB` / busy.
- **High VGA can silently stop streaming** on this firmware: at LNA 40,
  VGA ≥ 28 was accepted but delivered no bytes. Keep VGA ≤ 26 by default and
  detect "streaming but zero bytes" as an error, not as silence.

### Antenna length cheat-sheet (quarter-wave ≈ 7500 / f_MHz cm)

| Band | Whip setting |
|---|---|
| HF 5–30 MHz (shortwave, CB) | Fully extended (still far too short — expect weak reception) |
| 88–137 MHz (FM broadcast, airband) | Fully extended |
| 144–174 MHz (2 m ham, marine) | ~46 cm, about 4 stages |
| 430–470 MHz (70 cm, PMR446) | ~17 cm, about 1½–2 stages |
| 800–900 MHz | Collapsed / base stub only |

Being near a window or outdoors matters more than exact length.

---

## 3. The stack

| Layer | Choice | Why |
|---|---|---|
| Language | **C++20** | Max performance; destructors clean up automatically (RAII); `std::thread` / `std::atomic` built in. |
| Build | **CMake + Ninja** | Standard C++ build; works with clangd/IDE tooling. |
| Environment | **Nix flake dev shell** (`flake.nix`, `nix develop`) | NixOS host; pins compiler, libraries and tools in one file. Verify attribute names with `nix search nixpkgs <name>`. |
| Device | **libhackrf** directly (no SoapySDR) | HackRF-only project; its RX callback hands us USB buffers with no extra layer. |
| DSP | **liquid-dsp** | Fast C library: FIR filters, NCO (digital tuning), resamplers, AM/FM demodulators, and a polyphase channelizer for multi-channel later. |
| Spectrum | **FFTW3** (float, `fftwf_*`) | Fastest FFT; used for signal level / future waterfall. |
| Audio | **miniaudio** (single header, vendored) | Low-latency output through PipeWire/PulseAudio/ALSA, no system dependency. |
| WAV | **dr_wav** (single header, vendored) | Recording and test fixtures. |
| Channel list | **toml++** (single header, vendored) | Human-readable `channels/istanbul.toml`. |
| CLI parsing | **CLI11** (single header, vendored) | Subcommands + flags with help text. |
| Station screen | **FTXUI** (from the Nix flake) | Terminal UI with layout, buttons and mouse clicks. Added 2026-10-11 because the hand-written escape-code screen had no mouse support. |
| Tests | **doctest** + recorded `.iq` files | Fast compiles; tests never need the radio. |
| Quality | **ASan + UBSan** in debug, **clang-tidy**, **clang-format** | Catch memory and undefined-behaviour bugs immediately. |

Single-header libraries live in `third_party/` with their licence and version noted.

**Reference tools (not part of our code):** `hackrf_info`, `hackrf_transfer -r`
to capture test I/Q, and `SDR++` to confirm a signal is on air before
debugging our code.

### Threading model (the core of the performance design)

```
libhackrf USB thread ──► SPSC ring (raw int8 I/Q) ──► DSP thread ──► SPSC ring (float audio) ──► miniaudio callback
                                                          │
                                                          └──► stats snapshot (levels, FFT) ──► CLI status line / future UI
```

- **SPSC ring** = single-producer / single-consumer lock-free queue. No mutex
  on the signal path.
- The **libhackrf callback only copies** into the ring and returns. No
  allocation, no logging, no DSP there — blocking it drops USB samples.
- The **audio callback only reads** from the ring. Underrun → output silence
  and count it; never block.
- **No heap allocation after start-up** in the DSP thread: buffers and
  liquid-dsp objects are created once.
- Any frontend (TUI, web, GUI — decided later) only reads the stats snapshot,
  so it can never cause audio dropouts.
- Count and report: dropped USB buffers, audio underruns, clipped samples.

---

## 4. Planned layout (create as we go — don't scaffold everything at once)

```
vocatron/
├── AGENTS.md
├── flake.nix                  # dev shell: cmake, ninja, clang tools, hackrf, liquid-dsp, fftw
├── CMakeLists.txt
├── CMakePresets.json          # debug (ASan/UBSan) and release (-O3 -march=native) presets
├── channels/
│   └── istanbul.toml          # the frequency plan below
├── third_party/               # vendored single headers: miniaudio, dr_wav, toml++, CLI11, doctest
├── src/
│   ├── core/                  # libvocatron: the engine, no I/O to the terminal
│   │   ├── ring.hpp           # SPSC lock-free ring buffer
│   │   ├── channels.{hpp,cpp} # load/validate channel TOML
│   │   ├── source.{hpp,cpp}   # HackRfSource (libhackrf) + FileSource (.iq files)
│   │   ├── dsp.{hpp,cpp}      # NCO shift, decimation, AM/NFM/WFM, de-emphasis, AGC
│   │   ├── squelch.{hpp,cpp}  # channel-power gate
│   │   ├── audio.{hpp,cpp}    # miniaudio sink + WAV sink
│   │   └── engine.{hpp,cpp}   # owns threads, wires source → dsp → sinks
│   └── cli/
│       └── main.cpp           # `vocatron` executable
├── samples/                   # small .iq captures (large ones gitignored)
└── tests/
```

The engine (`src/core`) is a library so a TUI/web/GUI frontend can be added
later without touching the DSP.

Target CLI (first milestones):

```
vocatron list --group airband
vocatron listen LTFM_ATIS
vocatron listen 156.800 --mode nfm --record ch16.wav
vocatron listen POWER_FM --lna 16 --vga 20
vocatron demod samples/fm_100M.iq --mode wfm --out test.wav   # offline, no radio
```

While listening, print one updating status line: frequency, mode, signal
level (dBFS), clip %, dropped buffers, audio underruns.

---

## 5. DSP plan (the signal chain)

```
HackRF (LO = channel − 250 kHz, 2.4 MS/s, int8 I/Q)
  → convert to std::complex<float>
  → NCO frequency shift +250 kHz (move channel to 0 Hz)
  → low-pass FIR + decimate to channel rate
  → demodulate (AM / NFM / WFM)
  → de-emphasis (FM) / DC removal + AGC (AM)
  → audio filter + resample to 48 kHz → miniaudio / WAV
```

Key decisions and the reason for each:

- **Default sample rate 2.4 MS/s**, baseband filter 1.75 MHz. Divides cleanly:
  ÷10 → 240 kHz (WFM), ÷50 → 48 kHz (AM/NFM). Higher rates (8–20 MS/s) only
  for the later multi-channel mode.
- **Offset tuning:** HackRF has a DC spike exactly at the centre frequency.
  Tune the hardware 250 kHz away and shift digitally.
- **Filters are stateful objects that live for the whole session** (liquid-dsp
  objects keep their history). Re-creating them per block causes clicks.
- `float` / `std::complex<float>` everywhere, never `double` on the hot path.
- Build release with `-O3 -march=native`; let liquid-dsp/FFTW use SIMD.
- Default gains: amp **off**, LNA 16–24 dB, VGA 20 dB (max 26 by default, see
  quirks). Expose as CLI flags.

### Modes

| Mode | Used for | Channel BW | Notes |
|---|---|---|---|
| `am` | Airband, shortwave, CB (often) | 8–10 kHz | Envelope → remove DC → AGC |
| `nfm` | Marine, ham repeaters, PMR446, CB (some) | 12.5 or 25 kHz | FM discriminator; ±2.5 kHz (12.5 k) or ±5 kHz (25 k) deviation |
| `wfm` | FM broadcast, analog wireless mics | ~200 kHz | FM discriminator; **50 µs de-emphasis** (Europe/Türkiye — not the US 75 µs) |

SSB (`usb`/`lsb`) is not in scope at first; add only if needed for HF/CB.

---

## 6. Frequency plan (to become `channels/istanbul.toml`)

Each entry: `name`, `freq_mhz`, `mode`, `bw_khz`, `group`, `description`.

### Airband — AM, 8.33/25 kHz — whip full
| Name | MHz | Notes |
|---|---|---|
| LTFM_ATIS | 128.200 | 24/7 loop — **best first test signal** |
| LTFJ_ATIS | 129.825 | 24/7 loop |
| IST_APP | 118.950 | Istanbul Approach, busiest |
| IST_RADAR | 120.500 | Director / final vectors |
| IST_CTL | 121.100 | En-route |
| LTFM_TWR | 124.775 | Istanbul Airport tower |
| LTBA_TWR | 118.100 | Atatürk tower |
| LTBA_GND | 121.700 | Ground |
| LTBA_DEL | 121.900 | Delivery / apron |
| IST_INFO | 125.850 | VFR information |
| GUARD | 121.500 | Emergency only, usually silent |

Aircraft are heard far better than ground stations (line of sight).

### Marine VHF — NFM 25 kHz — whip ~46 cm
| Name | MHz | Notes |
|---|---|---|
| CH16 | 156.800 | Distress/calling |
| CH11_YESILKOY | 156.550 | Istanbul VTS |
| CH12_MODA | 156.600 | Istanbul VTS |
| CH13_KANDILLI | 156.650 | Istanbul VTS |
| CH14_TURKELI | 156.700 | Istanbul VTS |
| CH67_SG | 156.375 | Coast Guard / SAR |
| CH08 | 156.400 | Ship-to-ship |

### Amateur 2 m / 70 cm — NFM 12.5 kHz (try 25 kHz if distorted)
| Name | MHz | Whip |
|---|---|---|
| R0 | 145.600 | ~46 cm |
| R1 | 145.625 | ~46 cm |
| R4 | 145.700 | ~46 cm |
| R7 | 145.775 | ~46 cm |
| RU1 | 439.150 | ~17 cm |
| RU5 | 439.250 | ~17 cm |
| RU7 | 439.300 | ~17 cm |
| ISS_SSTV | 145.800 | Event-only; record to WAV, decode images with QSSTV (outside this app) |

Some repeaters may carry digital voice (DMR/C4FM) — that sounds like noise and
is out of scope.

### PMR446 — NFM 12.5 kHz — whip ~17 cm
Ch1–Ch8: 446.00625, 446.01875, 446.03125, 446.04375, 446.05625, 446.06875,
446.08125, 446.09375 MHz. All 8 fit inside 100 kHz → good scan-mode target.

### FM broadcast — WFM 200 kHz — whip full
TRT_FM 91.400 · KRAL_FM 92.000 · TRT_RADYO1 95.600 · POWER_FM 100.000 ·
METEOROLOJI 103.000 · RADYO_TRAFIK 104.200 · VIRGIN 106.200
**Strongest signals — use these to test the WFM chain first.** If audio
distorts, *lower* the gain (overload), don't raise it.

### CB 27 MHz — AM (try NFM too) — whip full
CH14 27.125 · CH19 27.185 · CH22 27.225. Weak with this antenna.

### Shortwave HF — AM, 5–9 kHz — whip full, expect weak results
| Name | MHz | Notes |
|---|---|---|
| TRT_VOT | 5.960 | Verify current schedule |
| SW_6005 / SW_7310 | 6.005 / 7.310 | Verify |
| SW_9460 / SW_9770 | 9.460 / 9.770 | Verify |
| WWV_10 / WWV_15 | 10.000 / 15.000 | WWV/WWVH (USA) — hard from Istanbul |
| RWM_10 | 9.996 | Russian time signal, beeps only |

Correction to the original notes: **CHU (Canada) is on 3.330 / 7.850 / 14.670 MHz,
not 10/15 MHz**, and RWM is on 4.996 / 9.996 / 14.996 MHz. Shortwave schedules
change often — check EiBi or short-wave.info before assuming a station is on air.

### Wireless microphones — WFM — optional, low priority
470–694 MHz, 823–832 MHz, 863–865 MHz. Many modern systems are digital
(silent/noise to us). **Ethics/legal:** these carry private events and
meetings. Turkish law (and common sense) forbids recording or sharing private
communications. Code may support tuning here; it must never auto-record
this group.

### Out of scope (not audio)
DVB-T2, PAL TV, 5.8 GHz FPV video, ADS-B (1090 MHz), and anything involving TX.

---

## 7. Milestones

1. **Env:** `flake.nix` dev shell; `cmake --preset debug && cmake --build` builds a hello-world that calls `hackrf_init` and prints the board info.
2. **Capture:** `vocatron capture 100.0 --seconds 5 -o samples/fm.iq` (RX only), plus FFT peak printout to prove the signal is there.
3. **Ring + threads:** SPSC ring with a unit test; source → DSP thread → counter, no drops at 2.4 MS/s.
4. **WFM offline:** `vocatron demod samples/fm.iq --mode wfm --out fm.wav` produces listenable audio. doctest covers it.
5. **WFM live:** `vocatron listen POWER_FM` plays through speakers with the status line.
6. **AM:** `vocatron listen LTFM_ATIS` — readable ATIS.
7. **NFM:** marine CH16 / PMR446 / repeaters.
8. **Channels TOML + `list`, recording** (`--record`).
9. Squelch is in (`--squelch`, status `sql shut` or `sql open`). The simple station screen is `vocatron tui`. Still later: `scan` a group, multi-channel via polyphase channelizer, HF tuning.

Finish and verify one milestone before starting the next.

---

## 8. Conventions

- C++20, `-Wall -Wextra -Wpedantic -Werror` in debug. Format with `clang-format` before committing; keep `clang-tidy` clean.
- Debug builds always run with ASan + UBSan. Tests run under the debug preset.
- RAII wrappers for every C handle (`hackrf_device*`, liquid-dsp objects, FFTW plans, miniaudio device). No naked `new`/`delete`, no manual `free` in business code.
- Frequencies in code are **Hz as `uint64_t`** (`128'200'000`); MHz only in the TOML file and on the CLI.
- No exceptions or allocation on the hot path (USB callback, DSP loop, audio callback). Errors there become counters/flags.
- DSP stages are classes with `process(std::span<const In>, std::span<Out>)` so they test without hardware.
- Every hardware-touching class has a file-based stand-in.
- Name magic numbers (`constexpr float kDeemphTau = 50e-6f;`, `constexpr uint32_t kLoOffsetHz = 250'000;`).
- Header comments say *what the DSP step does and why*, in plain words.

## 9. Working with the beginner owner

- Before writing a DSP function, explain in 2–4 sentences what it does to the signal.
- After a change, give the exact command to try and what they should hear/see.
- When something "doesn't work", first check: right mode? antenna length? gain too
  high (distortion) or too low (hiss)? signal actually on air (check in SDR++)?
- Prefer one clear way of doing something over configurable abstractions.

## 10. Glossary

- **I/Q samples** — complex numbers describing the radio signal around the tuned frequency.
- **LO / centre frequency** — where the HackRF is physically tuned.
- **Decimation** — filter, then keep every Nth sample, to lower the sample rate.
- **AM / NFM / WFM** — amplitude modulation / narrow FM (voice radios) / wide FM (broadcast).
- **De-emphasis** — treble cut that undoes the treble boost FM stations add.
- **Squelch** — mute audio when the channel is silent.
- **Overload** — signal too strong for the receiver; fixes: lower gain, amp off.
- **NCO** — numerically controlled oscillator; a digital tuner that shifts the spectrum.
- **Polyphase channelizer** — splits one wide capture into many narrow channels at once, efficiently.
- **SPSC ring buffer** — fixed-size queue between exactly two threads, no locks needed.
- **RAII** — C++ pattern: an object frees its resource (device, memory) automatically when it goes out of scope.
- **ASan / UBSan** — compiler checks that stop the program at the first memory or undefined-behaviour bug.
