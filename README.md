<p align="center">
  <img src="docs/vocatron.png" alt="vocatron: SDR listening software for ATC, VHF and FM" width="578">
</p>

<p align="center">
  <b>Listen to air traffic control, ships, ham radio and FM broadcast with a HackRF.</b><br>
  Receive only · C++20 · Linux · terminal UI with mouse support
</p>

---

vocatron turns a HackRF One (or a PortaPack in HackRF mode) into a radio
you can actually listen to. Pick a station from a list, or let vocatron
search a band and find what is on the air. It tunes, filters and
demodulates the signal and plays it on your speakers, with squelch so
empty channels stay quiet.

**Receive only.** vocatron never transmits. The only streaming call it
makes is `hackrf_start_rx`; it never switches on antenna power (bias tee).
The RF amplifier is off unless you turn it on yourself.

## What it can do

- **Play AM, narrow FM and wide FM**: airband, marine VHF, 2 m and 70 cm
  ham, PMR446, CB, shortwave, and FM broadcast.
- **Search any band.** Watch airband, marine, ham, PMR, CB or shortwave
  and list every channel someone talked on, or scan the FM band for every
  station you can receive.
- **Scan list.** Hop through a list of channels and stop on the one that
  starts talking, like a hardware scanner. The easy way to follow a tower or
  an approach controller.
- **RDS station names** on FM broadcast (for example `TRT FM`, `FENOMEN`).
- **Clarity gauge** for FM: how far a station sits above FM's noise cliff,
  measured from the noise riding on it, so it does not change with gain.
  A gentle treble cut takes the hiss off weak stations.
- **Save, name and rename** the stations you can hear. They are kept in
  `channels/saved.toml` and show your names everywhere.
- **Hides interference.** Steady carriers from nearby electronics (an
  evenly spaced comb, or the radio's own clock on whole megahertz) are
  recognised and left out of search results.
- **Record** to WAV, and **demodulate** saved I/Q captures without a radio.

## Quick start

```sh
git clone https://github.com/Arcioth/vocatron
cd vocatron
nix develop                                   # compiler, CMake, libraries
cmake --preset debug && cmake --build --preset debug
./build/debug/vocatron tui                    # the station screen
```

Without Nix, see [Building without Nix](#building-without-nix).

On a PortaPack, choose **HackRF** in the main menu first, then plug in USB.
`./build/debug/vocatron info` should print the board.

## The station screen

`vocatron tui` works with the mouse or the keyboard.

```
╭ vocatron  All│Saved│Found│Airband│FM│Marine│Ham│PMR│CB│Shortwave ───────────────╮
│    LTBA_ATIS       128.200          │LTBA_TWR                                    │
│    IST_APP         118.950          │119.800 MHz · AM · airband                  │
│> ▶ Atatürk Kule    119.800 ★        │Signal  ██████████████████▍       -48.2 dBFS│
│    LTBA_GND_N      121.600          │Sound   playing                             │
│    ...                              │Gain      −   40 dB   +                     │
│                                     │VGA       −   26 dB   +                     │
│                                     │RF amp    OFF                               │
│                                     │Squelch   −    -52 dBFS   +     gate on     │
│                                     │ Forget ★    Rename    Scan list            │
│                                     │Search  FM│AIR│MAR│2M│70CM│PMR│CB│SW49 Search│
╰─────────────────────────────────────┴────────────────────────────────────────────╯
```

**Left:** stations, with tabs on top. *Saved* holds yours, *Found* holds
the last search. Click a station to listen.

**Right:** what is playing and the controls.

| Control | What it does |
|---|---|
| Gain − / + | LNA, the main amplifier, 0 to 40 dB in steps of 8. Raise it when a station is buried in hiss |
| VGA − / + | Second amplifier, 0 to 26 dB in steps of 2 |
| RF amp | About 11 dB more, right at the antenna. Off at start, shown in red when on, and it turns itself off if the input starts clipping |
| Squelch − / + and gate | Mutes the speaker while the channel is quieter than the level. On for AM (−52 dBFS) and narrow FM (−48). Off for broadcast FM, which never pauses |
| Save | Name the playing station and keep it (★). Suggests the RDS name when there is one |
| Rename, Forget | For a saved station: give it another name, or drop it |
| Scan list | Hop through the AM / narrow FM stations in the current tab and stop on the one that talks |
| Search + band | Find what is on a band (pauses listening while it runs) |
| Mute, Stop/Play, Quit | As named |

While a wide FM station plays you also see **Clarity** (0 to 1) and its
**RDS** name. The small line at the bottom shows the converter level,
clipping, dropped USB bytes and audio gaps; clip and gaps should stay at 0.

Keyboard: arrows and Enter pick a station, `=` / `-` change the gain,
`[` / `]` move the squelch, `s` toggles the gate, `m` mutes, Esc stops,
`q` quits.

### Finding stations

Pick a band next to **Search** and click it.

- **FM** takes about a minute. It lists every station the radio can hear
  with four clarity dots (●●●● is clean) and the RDS name when one comes
  through. A live run from Istanbul found about 60 stations.
- **Any other band** is watched for 45 seconds, because towers, ships and
  handhelds only talk now and then. Each channel in the *Found* tab is
  tagged `talk ×N` (heard N times) or `steady` (on the whole time, such as
  an ATIS loop). A channel already in your list keeps its name.
  **Stop search** keeps what was found so far.

Then click a channel to listen, **Save** the ones you can hear, or press
**Scan list** to catch the next call. Scan list stops when a channel is
over the gate *and* 8 dB over the quietest level it has seen on that
channel, moves on after 3 seconds of quiet, and skips steady carriers.

### Custom frequencies

The **Custom** box takes frequencies that are not in the station list.

- One or several: `145.625` or `145.500, 145.625, 439.150`
- A grid: `144-146/12.5` means 144 to 146 MHz, every 12.5 kHz (at most 128)

Pick **AM**, **NFM**, or **WFM**, then **Add**. They show on the Custom
tab and in `channels/custom.toml` (that file stays on this machine).
**Scan list** on that tab walks them and stops when one talks. **Drop**
removes the highlighted one. A restricted frequency is stored, but Scan
list skips it until you accept the warning.

## Command line

| Command | What it does | Radio |
|---|---|---|
| `info` | Print the connected HackRF | yes |
| `list [--group airband]` | Print the station list | no |
| `listen <name or MHz>` | Play a station, optionally record it | yes |
| `scan` | Find the FM stations you can receive, with clarity and RDS | yes |
| `search <band>` | Watch a band and list the channels in use | yes |
| `custom [frequencies]` | Add or print the custom scan list | no |
| `tui` | The station screen | yes |
| `capture <MHz>` | Record raw I/Q and print where the energy is | yes |
| `demod <file.iq>` | Demodulate a capture to a 48 kHz WAV | no |
| `selftest` | Check the spectrum maths on a generated tone | no |
| `ringtest` | Stream 2.4 MS/s and count dropped bytes | yes |

```sh
./build/debug/vocatron listen LTBA_ATIS --lna 40 --vga 26
./build/debug/vocatron listen 156.8 --mode nfm --squelch -48
./build/debug/vocatron listen CH16 --seconds 30 --record ch16.wav
./build/debug/vocatron scan --lna 32 --vga 26 --amp
./build/debug/vocatron search airband --seconds 60
./build/debug/vocatron custom 145.625,439.150 --mode nfm
./build/debug/vocatron demod samples/fm.iq --mode wfm --out fm.wav
```

| Flag | Applies to | Meaning |
|---|---|---|
| `--lna N` | listen, capture, ringtest, scan, search | LNA gain, 0–40 in steps of 8 |
| `--vga N` | same | VGA gain, even, 0–26 |
| `--amp` | listen, capture, scan, search | RF amp on. Off unless given |
| `--seconds N` | listen, capture, ringtest, search | Run time. `listen` runs until Ctrl-C without it |
| `--mode wfm\|am\|nfm` | listen (typed frequency), demod | Demodulator |
| `--squelch DBFS` | listen | Mute while the channel is quieter than this. Off unless given |
| `--out FILE`, `--record FILE` | listen | Write a WAV (needs `--seconds`) |
| `-o FILE` | capture | Output `.iq` file |
| `--no-speakers` | listen | Do not open the sound device |
| `--group NAME` | list | One group only |

### Bands for `search`

| Band | Range | Grid | Mode |
|---|---|---|---|
| `airband` | 118.000–136.975 MHz | 8.33 kHz | AM |
| `marine` | 156.000–162.025 MHz | 25 kHz | narrow FM |
| `2m` | 144–146 MHz | 12.5 kHz | narrow FM |
| `70cm` | 430–440 MHz | 12.5 kHz | narrow FM |
| `pmr` | 446.006–446.194 MHz | 12.5 kHz | narrow FM |
| `cb` | 26.965–27.405 MHz | 10 kHz | AM |
| `sw49`, `sw41`, `sw31`, `sw25` | shortwave broadcast bands | 5 kHz | AM |

FM broadcast is `vocatron scan`.

## Hardware notes

- **HackRF One or PortaPack** (on a PortaPack, choose *HackRF* in the main
  menu). On NixOS you need `hardware.hackrf.enable = true;` and your user
  in the `plugdev` group.
- **"Resource busy"**: the kernel's `hackrf` module grabbed the device.
  Run `sudo modprobe -r hackrf`, or blacklist it. vocatron prints this hint.
- **VGA above 26** is accepted by some firmware and then streams nothing,
  so vocatron stops at 26 and treats "streaming but zero bytes" as an error.
- **The RF amp** adds about 11 dB at the antenna. The HackRF's input is
  damaged above about −5 dBm and big cities have strong FM and TV
  transmitters, so it stays off until you turn it on, and the screen turns
  it off again if the input clips.
- **USB cable**: an adapter chain (USB-A to C converter plus a C-to-C cable)
  raised the airband noise floor by about 5 dB in testing. A single direct
  cable is better.
- **Antenna**: a quarter-wave whip is about `7500 / MHz` centimetres. A
  short telescopic whip still receives strong stations well; for weak ones,
  position (a window, outdoors, away from the computer) matters most.

## How it works

```
USB thread ──► SPSC ring (raw 8-bit I/Q) ──► DSP thread ──► SPSC ring (48 kHz audio) ──► sound card
                                                │
                                                └──► levels, clarity, RDS ──► status line / screen
```

The USB callback only copies bytes into a lock-free queue. The DSP thread
does all the work, and the sound card callback only reads; playback keeps a
150 ms cushion and refills it if the radio's audio ever runs late. The
screen reads a snapshot of the counters, so drawing can never cause a
dropout. Changing station or gain retunes the running radio instead of
reopening it.

Every mode starts the same way: the radio is tuned 250 kHz below the
station (the HackRF has a spike at its exact centre), samples at 2.4 MS/s,
and the station is shifted back to 0 Hz in software.

| Mode | Channel | Demodulation | Audio |
|---|---|---|---|
| AM | two steps to 48 kHz; passes ±2.2 kHz, −60 dB by ±5.8 kHz | envelope, carrier removed, slow volume control | 2.7 kHz voice filter |
| Narrow FM | ±8 kHz | FM discriminator, ±5 kHz ≈ full scale | 750 µs de-emphasis, 3.4 kHz voice filter |
| Wide FM | ±80 kHz | FM discriminator | 50 µs de-emphasis, 15 kHz filter (drops the 19 kHz pilot), weak-signal treble cut, RDS |

Each mode has its own speaker gain, so a normal voice plays at about the
same loudness (around −22 dBFS) whatever the mode.

- **Clarity** measures the demodulated FM signal above 75 kHz, where no
  program lives, so whatever is there is noise. It tracks signal-to-noise
  and ignores signal strength.
- **RDS** mixes the 57 kHz subcarrier down, recovers symbol timing and
  carrier phase, undoes biphase and differential coding, and finds blocks by
  their checksums. Sync needs two good blocks in a row, so noise cannot
  invent names.
- **FM scan** surveys the band with an FFT in 0.5 MHz steps, then records
  2.5 s per tuner position and decodes every candidate in it.
- **Band search** runs the radio at 20 MS/s so one tuning covers about
  7 MHz, sweeps the band about ten times a second, and keeps every visit for
  every channel. A channel's usual level is the median of its visits.
  *Talk* means it jumped at least 10 dB above that, at least twice.
  *Steady* means its usual level is at least 8 dB above the band's.

## Channel files

`channels/istanbul.toml` is the built-in list. Each station is a block:

```toml
[[channel]]
name = "LTBA_TWR"
freq_mhz = 119.800
mode = "am"           # am, nfm or wfm
bw_khz = 8.33
group = "airband"     # airband, broadcast, marine, amateur, pmr, cb, shortwave
description = "Ataturk tower"
```

The airband entries were checked against SkyVector (from the Turkish AIP)
in October 2026. Frequencies change, so check before relying on one.

`channels/saved.toml` holds the stations you saved, in the same format. It
is personal, so git ignores it. It is safe to edit by hand.

## Building without Nix

You need a C++20 compiler, CMake 3.25+, Ninja, pkg-config and:

- libhackrf, liquid-dsp, FFTW3 (single precision), FTXUI 6
- PulseAudio or PipeWire's PulseAudio layer at run time

miniaudio, toml++ and doctest are vendored in `third_party/`.

```sh
cmake --preset release && cmake --build --preset release   # -O3 -march=native
./build/debug/vocatron_tests                               # tests never touch the radio
```

The debug preset builds with `-Wall -Wextra -Wpedantic -Werror` and
AddressSanitizer + UBSan.

## Project layout

```
src/cli/      main.cpp (commands), tui.cpp (station screen), tui_model.hpp (knobs, names)
src/core/     listen   live receiver: threads, retune, squelch, recording
              dsp      AM / narrow FM / wide FM chains, clarity, WAV writer
              rds      RDS decoder
              scan     FM scan and band search, interference detection
              bands    band list: range, grid, mode
              saved    saved stations
              playback, ring, rx, capture, channels, squelch, hackrf_session
tests/        doctest cases with generated signals: no radio needed
channels/     istanbul.toml (and your saved.toml)
docs/         logo
```

`AGENTS.md` is the design spec, `HANDOFF.md` records what was measured on
real hardware, and `CHANGELOG.md` lists changes by date.

## Status and limits

Tested on a HackRF One r9 clone in a PortaPack H2-style case, from
Istanbul: AM airband (ATIS and live approach/tower calls), wide FM with RDS
on dozens of stations, band search and scan list on airband. Narrow FM is
tested on generated signals and empty channels, but no live voice yet.

- One station plays at a time; a search pauses listening.
- No SSB, no FM stereo, no radio text. RDS shows the station name only.
- Linux only. Audio goes through PulseAudio (or PipeWire's layer).

## Legal

vocatron only receives. Listening rules differ by country, and you are
responsible for following the law where you use it. vocatron refuses to
record the `wireless` (wireless microphone) group. Never modify it to
transmit, least of all on aviation or marine frequencies.

Public-safety ranges live in `channels/restricted.toml`. Marine channel 67
(coast guard / search and rescue, 156.375 MHz) is the first entry, and it
is not in the station list. The station screen and `vocatron listen` will
not tune into a listed range until you confirm that you hold the licence
and permission and that you take all responsibility. A confirmation lasts
for that session only. Recording a restricted frequency is always refused.

### Haberleşmenin Gizliliği (TCK Madde 132 ve 133)

Havada şifresiz dolaşan her sinyali dinlemek yasal olarak serbest değildir.

**Yasal olan dinlemeler:** Kamuya açık radyo yayınları, deniz hava
durumu/navtex yayınları ve amatör telsizcilerin herkese açık hobi/afet
haberleşmeleri (amatör bantlar) serbestçe dinlenebilir.

**Suç teşkil eden dinlemeler:** Polis, jandarma, sahil güvenlik, itfaiye ve
ambulans gibi kamu güvenliği haberleşmelerini; şirketlerin veya şahısların
kapalı devre telsiz görüşmelerini dinlemek, şifreli haberleşmeleri kırmak,
dinlenen kayıtları depolamak ya da üçüncü şahıslarla paylaşmak Türk Ceza
Kanunu kapsamında haberleşmenin gizliliğini ihlal suçudur.

### Privacy of communications in Turkey (Turkish Penal Code, articles 132 and 133)

Not every unencrypted signal in the air is legal to listen to.

**Legal to listen to:** public radio broadcasts, marine weather and NAVTEX
broadcasts, and radio amateurs' open hobby and disaster communications
(the amateur bands).

**A crime:** listening to public-safety communications such as police,
gendarmerie, coast guard, fire and ambulance; listening to the closed
radio conversations of companies or individuals; breaking encrypted
communications; storing recordings of what was heard; or sharing them with
third parties. Under the Turkish Penal Code these violate the privacy of
communications.

## License

Copyright 2026 Arcioth.

vocatron is licensed under the [Apache License, Version 2.0](LICENSE). You
may use, change and share it, including in other projects, as long as you
keep the license and copyright notice and say what you changed.

The single-header libraries in `third_party/` keep their own licenses:
doctest and toml++ are MIT, miniaudio is public domain or MIT-0 (see
`third_party/VERSIONS.md`).
