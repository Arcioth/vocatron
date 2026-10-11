# vocatron

vocatron is a small C++20 program that turns a HackRF One (or a PortaPack in
HackRF mode) into a voice receiver on Linux. It tunes a channel, filters and
demodulates it, and plays the audio on your speakers. It handles AM (airband),
narrow FM (marine, PMR446, amateur) and wide FM (broadcast). It also has
squelch, WAV recording, offline demodulation of `.iq` captures, and a terminal
station screen (`vocatron tui`). The channel list in `channels/istanbul.toml`
is a frequency plan for Istanbul, but any frequency can be tuned by hand.

**Receive only.** vocatron never transmits. The code does not call any
transmit function and does not switch on antenna power (bias tee). The only
streaming call it uses is `hackrf_start_rx`.

The HackRF RF amplifier is off by default and only turns on with an explicit
`--amp` flag. The HackRF's maximum safe input is about -5 dBm, and Istanbul
has very strong FM and TV transmitters, so an always-on amp risks overload and
damage. Gain is set with the LNA and VGA stages instead.

## Status

This is a young project. Here is what has and has not been checked.

Verified on real hardware (HackRF One r9 clone, firmware `n_260928`):

- Live AM on 128.200 MHz was heard clearly. The listener held a hand around
  the antenna, and amp stayed off.
- Streaming at 2.4 MS/s through the sample queue dropped nothing, including a
  3 s `ringtest` and live `listen` runs.
- `info`, `capture`, `ringtest`, `listen` and `tui` run on the device.

Tested only on synthetic signals (generated test tones in the unit tests):

- Wide FM. No FM broadcast was received with the antenna tried so far. Live
  captures at 100 MHz demodulate to hiss.
- Narrow FM. Live tunes of 156.800 MHz (marine channel 16), 446.006 MHz
  (PMR1) and 439.150 MHz found no voice traffic.

Known gaps:

- No `scan` command. Only one channel plays at a time.
- `bw_khz` in the channel file is parsed but not used. Every narrow FM channel
  uses the same filter (about +-8 kHz).
- The command-line parser is hand-written. There are no `--help` details per
  subcommand, only one usage text.
- No SSB, no FM stereo or RDS, no multi-channel listening.
- Some channel names in the file are probably wrong. 128.200 MHz is labeled
  `LTFM_ATIS` but the Turkish AIP lists it as the Yesilkoy / LTBA loop. See
  `HANDOFF.md`.
- Linux and x86_64 only. The flake targets `x86_64-linux`.
- Audio output uses PulseAudio (or PipeWire's PulseAudio layer) through
  miniaudio.

## Hardware

- HackRF One, or a PortaPack. On a PortaPack with Mayhem firmware, choose
  **HackRF** in the main menu so it acts like a plain HackRF One over USB.
- `hackrf_info` (from the HackRF tools) should list the device before you try
  anything else. On NixOS this needs `hardware.hackrf.enable = true;` and your
  user in the `plugdev` group.

### Resource busy

Linux has a kernel module called `hackrf` that can claim the USB interface.
Then libhackrf fails to open the device with "resource busy". Unload it:

```
sudo modprobe -r hackrf
```

To make it permanent, blacklist the module in your system configuration.
vocatron prints this hint when the open fails.

### VGA limit

On the firmware tested, a VGA gain above 26 dB is accepted but the device
sends no data. vocatron only accepts even VGA values from 0 to 26, and it
treats a stream that delivers zero bytes as an error, not as silence.

### Antenna

A quarter-wave antenna is about `7500 / f_MHz` centimeters long. A telescopic
whip is a rough match at best. Starting points from the project notes:

| Band | Whip setting |
|---|---|
| HF 5-30 MHz (shortwave, CB) | Fully extended, still far too short |
| 88-137 MHz (FM broadcast, airband) | Fully extended |
| 144-174 MHz (2 m amateur, marine) | about 46 cm |
| 430-470 MHz (70 cm, PMR446) | about 17 cm |
| 800-900 MHz | Collapsed |

Position matters more than exact length. Near a window or outdoors is better
than deep inside a building. The whip used in testing only extends to about
25 cm, so it is short for everything below 300 MHz.

## Build

### With Nix

```
cd vocatron
nix develop
cmake --preset debug && cmake --build --preset debug
```

The dev shell provides cmake, ninja, pkg-config, clang-tools, libhackrf,
liquid-dsp and FFTW (float).

The debug preset builds with `-Wall -Wextra -Wpedantic -Werror` and
AddressSanitizer plus UBSan. For a faster build:

```
cmake --preset release && cmake --build --preset release
```

The release preset uses `-O3 -march=native`. Binaries land in
`build/debug/` or `build/release/`.

### Without Nix

You need a C++20 compiler, C99 compiler, CMake 3.25 or newer, Ninja,
pkg-config, and these libraries:

- libhackrf (found through pkg-config)
- liquid-dsp (linked as `-lliquid`)
- FFTW3, single precision (`fftw3f`, found through pkg-config)

miniaudio, toml++ and doctest are vendored in `third_party/`
(see `third_party/VERSIONS.md`). Audio playback loads libpulse at run time.
The Nix dev shell's loader does not search the system library path, so
vocatron also tries `/run/current-system/sw/lib/libpulse.so.0` and
`/usr/lib/libpulse.so.0` itself.

### Tests

The tests never touch the radio.

```
./build/debug/vocatron_tests
```

## Usage

Run `vocatron` with no arguments to print the board (same as `info`). The
examples below use the debug build path.

| Command | What it does | Opens the radio |
|---|---|---|
| `info` | Print the connected HackRF | yes |
| `selftest` | Check the spectrum math on a fake tone | no |
| `capture` | Record I/Q to a file and print a spectrum summary | yes |
| `ringtest` | Stream 2.4 MS/s through the sample queue and count drops | yes |
| `demod` | Turn a saved capture into a 48 kHz WAV | no |
| `listen` | Play a channel or frequency, optionally record | yes |
| `list` | Print channels from the channel file | no |
| `tui` | Station screen | yes |

```
# Is the radio there?
./build/debug/vocatron info

# Check the FFT code without the radio
./build/debug/vocatron selftest

# Record 5 s of raw int8 I/Q at 100.0 MHz (receive only)
./build/debug/vocatron capture 100.0 --seconds 5 -o samples/fm.iq --lna 40 --vga 26

# Count dropped USB bytes over 3 s
./build/debug/vocatron ringtest --seconds 3

# Demodulate a capture offline (capture must be 2.4 MS/s int8 I/Q)
./build/debug/vocatron demod samples/fm.iq --mode wfm --out samples/fm.wav

# List channels, or one group
./build/debug/vocatron list
./build/debug/vocatron list --group marine

# Listen to a named channel
./build/debug/vocatron listen LTFM_ATIS --lna 40 --vga 26

# Listen to a bare frequency (defaults to wfm unless --mode is given)
./build/debug/vocatron listen 156.8 --mode nfm

# Record 5 s to WAV, without speakers, with squelch
./build/debug/vocatron listen CH16 --seconds 5 --no-speakers --squelch -50 --record ch16.wav

# Station screen
./build/debug/vocatron tui
```

### Flags

| Flag | Applies to | Meaning |
|---|---|---|
| `--lna N` | capture, ringtest, listen | LNA gain in dB. Default 16 |
| `--vga N` | capture, ringtest, listen | VGA gain in dB, even, 0 to 26. Default 20 |
| `--amp` | capture, listen | Turn the RF amp on. Off unless you pass it |
| `--seconds N` | capture, ringtest, listen | Run time. `listen` runs until Ctrl-C without it |
| `-o`, `--out FILE` | capture | Output `.iq` file |
| `--out FILE`, `--record FILE` | listen | Write a WAV. Needs `--seconds` |
| `--out FILE` | demod | Output WAV |
| `--mode wfm\|am\|nfm` | demod (required), listen | Demodulator. For a named channel it must match the channel's mode |
| `--shift-khz N` | demod | Digital shift, default 250 |
| `--squelch DBFS` | listen | Mute while the channel is quieter than this level (-120 to 0). Off unless passed |
| `--no-speakers` | listen | Skip the sound device |
| `--group NAME` | list | Show one group |

If the loudest ADC sample reaches 127, you are overloading the front end.
Lower the LNA gain. Do not turn the amp on to fix a weak signal until you
know the input is not already strong.

### Squelch

`--squelch` watches the strength of the filtered channel in dBFS, not the
speaker output (empty FM is loud hiss, so audio level is a poor clue). The
gate opens at the level you give, stays open 0.3 s after the channel drops 4
dB below it, and fades over 10 ms. Muting applies to the speaker and to the
recording together. The status line shows `sql off`, `sql open` or `sql shut`.

Where the empty-channel level sits depends on gain. At LNA 40 / VGA 26 the
measured values were about -55 dBFS for empty narrow FM and about -34 dBFS
for wide FM noise, so useful levels differ by mode.

### Station screen

`vocatron tui` shows the channel list and plays the highlighted station. It
needs a terminal. It starts on LTFM_ATIS at LNA 40, VGA 20, amp off, speaker
on, with a squelch level chosen for the mode (AM -58, wide FM -32, narrow FM
-48 dBFS). The gate starts closed, so an empty channel stays silent.

| Key | Action |
|---|---|
| Up / Down, `k` / `j` | Move the highlight |
| Enter | Tune the highlighted station |
| `[` / `]` | Squelch looser / tighter by 2 dB (kept until the station changes) |
| `s` | Squelch on/off |
| `m` | Mute the speaker |
| `-` / `=` (or `+`) | LNA down / up by 8 dB, applied without restarting the radio |
| `g` | Cycle the group filter |
| Esc | Stop the radio |
| `q` | Quit |

## How it works

Three threads, with no locks on the signal path:

```
libhackrf USB thread --> SPSC ring (int8 I/Q) --> DSP thread --> SPSC ring (float audio) --> audio callback
                                                       |
                                                       +--> stats (level, clip, drops) --> status line / TUI
```

- The USB callback only copies bytes into a 4 MiB single-producer,
  single-consumer ring and returns. If the ring is full, bytes are dropped
  and counted.
- The DSP thread reads the ring, demodulates, and writes 48 kHz float audio
  into a second ring.
- The audio callback only reads. On underrun it plays silence and counts it.
- The status line and the station screen read a snapshot of the counters, so
  they cannot cause dropouts.

DSP chain, shared by all modes:

1. Tune the radio 250 kHz below the channel. The HackRF has a DC spike at its
   exact center frequency, and the offset keeps it off the channel.
2. Sample at 2.4 MS/s, 8-bit I/Q, with a 1.75 MHz hardware baseband filter.
3. Shift the signal by +250 kHz in software so the channel sits at 0 Hz.
4. Low-pass filter and decimate to the mode's rate (2.4 MS/s divided by 10
   gives 240 kHz for wide FM, divided by 50 gives 48 kHz for AM and narrow FM).
5. Demodulate, then clean up audio and write 48 kHz output.

Per mode:

| Mode | Demodulation | Audio cleanup |
|---|---|---|
| `am` | Envelope, keeps about +-3.5 kHz around the carrier | Remove the steady carrier, slow AGC (0.3 s), 2.7 kHz voice filter. First 50 ms is silent while the AGC settles |
| `nfm` | FM discriminator, keeps about +-8 kHz, +-5 kHz deviation is about full scale | 750 us de-emphasis, voice under 3.4 kHz. First 50 ms silent |
| `wfm` | FM discriminator on a 240 kHz channel | 50 us de-emphasis (Europe and Turkey, not the US 75 us) |

Filters are created once and keep their state for the whole session, because
rebuilding them per block causes clicks. Each mode has its own speaker gain
(AM 0.05, narrow FM 1.0, wide FM 0.25), so a normal voice plays at about
-22 dBFS RMS in all three and empty-channel hiss never clips. Without
squelch, an empty FM channel's hiss is louder than a voice; that is normal
for FM and is what the squelch is for.

In the station screen, a station change or LNA step keeps the radio and the
speakers open. The tuner moves, the demodulator is rebuilt, and about 0.1 s
of audio is muted while the new station settles.

## Channels

Stations live in `channels/istanbul.toml`. Add one by copying a block:

```toml
[[channel]]
name = "LTFM_ATIS"
freq_mhz = 128.200
mode = "am"
bw_khz = 8.33
group = "airband"
description = "Istanbul Airport ATIS, usually a loop"
```

| Field | Meaning |
|---|---|
| `name` | Name you pass to `listen` |
| `freq_mhz` | Station frequency in MHz. The radio is tuned 250 kHz below it |
| `mode` | `am`, `nfm` or `wfm` |
| `bw_khz` | Channel width. Recorded for reference, not yet used by the filters |
| `group` | Used by `list --group` and the `g` key in the station screen |
| `description` | Free text shown in the list and screen |

Groups in the shipped file: `airband` (11), `amateur` (8), `broadcast` (7),
`cb` (3), `marine` (7), `pmr` (8), `shortwave` (8).

There is a reserved `wireless` group name for wireless microphones. The
shipped file has no channels in it, and `listen` refuses to record a channel
in that group, because those carry private events and meetings.

Shortwave schedules change often. Check a schedule source before assuming a
station is on air.

## Project layout

```
vocatron/
├── AGENTS.md          spec, safety rules, conventions
├── HANDOFF.md         current state and measured results
├── CHANGELOG.md
├── CMakeLists.txt
├── CMakePresets.json  debug (ASan/UBSan) and release presets
├── flake.nix          Nix dev shell
├── channels/
│   └── istanbul.toml  channel list
├── src/
│   ├── cli/
│   │   ├── main.cpp       subcommands and flag parsing
│   │   ├── tui.cpp/.hpp   terminal station screen
│   │   └── tui_model.hpp  station screen state and key handling
│   └── core/
│       ├── capture.cpp/.hpp    info, capture, selftest, ringtest
│       ├── channels.cpp/.hpp   channel file loading
│       ├── dsp.cpp/.hpp        shift, decimation, AM/NFM/WFM, WAV writer
│       ├── hackrf_session.hpp  device open and identity
│       ├── listen.cpp/.hpp     live receive pipeline
│       ├── playback.cpp/.hpp   audio output (miniaudio)
│       ├── ring.hpp            lock-free SPSC ring
│       ├── squelch.cpp/.hpp    channel-power gate
│       └── miniaudio_impl.c, miniaudio_prefix.hpp
├── tests/             doctest cases (ring, am, nfm, wfm, channels,
│                      playback, squelch, tui)
├── third_party/       doctest.h, miniaudio.h, toml.hpp, VERSIONS.md
└── samples/           local captures (*.iq and *.wav are git-ignored)
```

## Legal and ethics

vocatron only receives. Listening to public broadcast, airband and amateur
traffic is generally fine, but rules differ by country. In Turkey, recording
or sharing private communications is forbidden. Do not record or publish
conversations that were not meant for the public, and do not use the wireless
microphone bands for anything but your own testing. Never transmit on
aviation or marine frequencies, and do not modify this program to do so. You
are responsible for following the law where you use it.

## License

Not yet chosen.
