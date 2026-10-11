# HANDOFF — vocatron

Receive-only HackRF voice receiver. Spec is `AGENTS.md`. Independent of Raphael.

## State (2026-10-09)

Milestones 1 through 8 are in place, and squelch is in on top of them.

`vocatron` with no arguments (or `info`) prints the board. `capture` tunes,
records signed 8-bit interleaved I/Q, and prints a spectrum. `selftest`
checks that spectrum math on a fake tone and does not open the radio.
`ringtest` streams through the sample queue and counts drops.
`demod` turns a capture into a 48 kHz WAV (`--mode wfm`, `am`, or `nfm`).
`listen POWER_FM` plays wide FM. `listen LTFM_ATIS` plays AM.
`listen CH16`, `RU1`, and `PMR1` play narrow FM. Names come from
`channels/istanbul.toml`. `list` prints them, and `list --group marine`
prints one group. Ctrl-C stops a listen. `--no-speakers` skips the sound
device. `--out` and `--record` write a WAV and both need `--seconds`.
`--squelch` mutes the speaker and the recording while the channel is quieter
than the level you pass. It is off unless you pass it. `vocatron tui` starts
on LTFM ATIS at LNA 40, VGA 20, amp off, with a gate chosen for the mode.

Verified on the plugged-in HackRF One r9 clone, firmware `n_260928`, API 1.13,
serial `0000000000000000674068dc27564763`, part id `0xa000cb3c 0xbc5d4738`.
The kernel `hackrf` module was loaded but did not block the open. No samples
were dropped. ASan/UBSan were on.

`selftest` reports the fake tone at 99.825 MHz and −6.0 dBFS. That is the
tone that was written.

## What 100.0 MHz actually looked like

Whip fully extended. Amp off unless noted. 2.4 MS/s, baseband 1.75 MHz,
LO 250 kHz below the channel so the DC spike is not on 100.0.

| Run | Gain | Loudest sample (of 127) | Result |
|---|---|---|---|
| `samples/fm_lna16.iq` (5 s) | LNA 16, VGA 20 | 6 | Quantization floor. Only the DC spike. |
| `samples/fm_lna40.iq` (5 s) | LNA 40, VGA 26 | 43 | No clipping. No FM-wide station. 100 kHz slices around 100.0 are about +5 dB over the median. |
| `samples/fm_amp.iq` (3 s) | amp ON, LNA 40, VGA 14 | 73 | No clipping. Same story. A mild wide bump near 99.2 MHz, about +7 dB. |

The narrow bins the program lists are ~300 Hz wide. A broadcast station is
~200 kHz wide, so those bins are not stations. The line that matters is
"channel … is N dB hotter than a quiet slice". On these captures that number
stayed under 8 dB.

The DC spike sat at −31.4 dBFS in every run. The receiver is producing I/Q.
The antenna input at this spot is not bringing in the FM band.

Amp was turned back off afterwards (a short LNA 16 / VGA 20 capture, file deleted).

## Whip length

The whip's longest setting is about 25 cm. A quarter-wave is about `7500 / f_MHz` cm.

| Frequency | Quarter-wave | This 25 cm whip |
|---|---|---|
| 100 MHz FM | 75 cm | About a third of the length. Weak on purpose. |
| 145–156 MHz (2 m, marine) | ~48–52 cm | About half. Still short. |
| 300 MHz | 25 cm | Full length is the match. |
| 439 MHz (70 cm) | 17 cm | A bit long. Collapse it toward 17 cm. |
| 900 MHz | 8 cm, or 25 cm as a 3/4 wave | Full length matches the 3/4 wave. |

Same gain (LNA 40, VGA 26, amp off), whip fully out:

| Tune | Loudest sample | Result |
|---|---|---|
| 100.0 MHz | 43, no clip | No FM station. |
| 439.150 MHz, whip ~25 cm (`samples/ru1.iq`, 3 s) | 12, no clip | Repeater channel empty (1.6 dB). Thin carrier near 440.000 MHz, −45 dBFS. |
| 439.150 MHz, whip ~17 cm (`samples/ru1_17cm.iq`, 3 s) | 20, no clip | Same tune, shorter whip. 440.000 MHz rose to −38 dBFS (34 dB over the noise, about 1 kHz wide). 439.200 MHz rose to −49 dBFS, also about 1 kHz wide. 439.150 ± 40 kHz had no bin 6 dB over the noise. |
| 940.0 MHz | clipped 15%, floor −39 dBFS | Whole window hot. IQ deleted. At LNA 8 / VGA 20 the same tune fell back to the quantization floor, so the loud result needs the high gain, and it is still far above the 100 MHz and 439 MHz captures at that same high gain. |

## Listening

The sample queue is 4 MiB. The USB callback only copies into it. A second
thread demodulates, and the speaker callback only reads a float queue.
Debug tests passed under ASan, including a paced 2.4 MS/s run with no drops.
A live `ringtest` of 3 s at 439.150 MHz counted 14,400,000 bytes and dropped 0.

Wide FM is checked on a made-up 1 kHz tone, 75 kHz deviation, at +250 kHz.
The tone comes back at least 20 times stronger than 300 Hz or 3 kHz.
`demod samples/fm_lna40.iq --mode wfm` writes 5.00 s of hiss. That capture
has no broadcast station, and the WAV is scaled so its peak is 40% of full scale.

`listen POWER_FM --seconds 1.2 --lna 40 --vga 26` opened PulseAudio, played,
dropped 0 USB bytes, and had 0 underruns. The ADC sat near −25 dBFS, the same
high-gain noise floor as the earlier 100 MHz capture. Playback gain is 0.05
so that hiss stays small. The nix dev-shell loader does not search the system
library path, so playback opens `/run/current-system/sw/lib/libpulse.so.0`
(or `/usr/lib/libpulse.so.0`) before miniaudio asks for it.

AM is the strength of the wave. The chain shifts by +250 kHz, keeps ±5 kHz,
keeps 1 of every 50 samples (48 kHz), removes the steady carrier, and runs a
slow volume control. The first 50 ms stays silent so that control can settle.
A made-up 1 kHz AM tone passes the same 20× check.

Narrow FM is the same phase measurement as wide FM, on a much smaller wiggle.
The chain keeps about ±8 kHz, turns a ±5 kHz wiggle into about full scale,
cuts treble with a 750 µs time constant (the one handheld and marine radios
use, slower than broadcast's 50 µs), then keeps voice under 3.4 kHz. The
first 50 ms stays silent while that settles. A made-up 1 kHz tone at 3 kHz
of wiggle passes the 20× check. A steady carrier stays near silence.
`bw_khz` in the channel file is written down (12.5 or 25) and is not yet
used to change this filter.

## What 128.200 MHz looked like

Whip still about 17 cm. That is short for airband. Amp off, LNA 40, VGA 26.

| Run | Result |
|---|---|
| `listen LTFM_ATIS --seconds 4 --no-speakers` | Dropped 0. ADC −26.8 dBFS, no clip. Demodulated audio level stayed flat (50 ms loudness varied by about 7%). |
| `samples/ltfm_atis.iq` (2 s) | Loudest sample 68, no clip. Channel only 1.9 dB hotter than a quiet slice. One narrow bin sits on 128.200 MHz at −51.3 dBFS, 10.6 dB over the noise. That is not a voice-wide station. |

Those two rows were before the hand-around-the-antenna listen. The next section replaces them.

## What LTFM ATIS sounds like (2026-10-09)

128.200 MHz is a real carrier. It was heard clearly with a hand closed around
the antenna, at every whip length. Collapsing the whip to the shortest stop
removed some of the noise. Amp stayed off. VGA stayed at 20.

| Capture | Gain | What came out |
|---|---|---|
| `samples/ltfm_now_lna16.iq` (3 s) | LNA 16 | Loudest sample 5. Narrow bin on 128.200 at −71.3 dBFS, 11.8 dB over the noise. Demod channel −69.1. Audio is flat (level variation 0.08). Squelch −58 stays shut, so this gain is silence instead of hiss. |
| `samples/ltfm_now_lna40.iq` (3 s) | LNA 40 | Loudest sample 14, no clip. Narrow bin on 128.200 at −50.1 dBFS, 27.1 dB over the noise, 0.1 kHz low. Demod channel −50.5. Speech is in the file: level variation 0.58, and the lows hold about 4× the hiss. |
| `samples/fm_now_lna40.iq` (2 s, 100.000 MHz) | LNA 40 | Not a wide FM station. Channel −41.4. The strongest energy is a narrow spike 352 kHz off tune. |

AM now keeps about ±3.5 kHz around the carrier, then a 2.7 kHz voice filter.
The volume control takes 0.3 s, so a gap between words is not turned up to
match the voice. The first 50 ms stays silent. Debug tests: 17 cases,
10159 assertions.

## What narrow FM looked like

Whip still about 17 cm. Amp off, LNA 40, VGA 26. Speakers stayed off.
17 cm matches 446 MHz. 156 MHz wants about 46 cm, and this whip cannot get there.

| Run | Result |
|---|---|
| `samples/pmr1.iq` (2 s, 446.006 MHz) | Loudest sample 26, no clip. Channel 1.9 dB hotter than a quiet slice. Not a station. |
| `listen PMR1 --seconds 2 --no-speakers` | Dropped 0. ADC −31.8 dBFS. Demod peak 0.77 before the 0.05 playback gain. |
| `samples/ch16.iq` (2 s, 156.800 MHz) | Loudest sample 15, no clip. Channel 1.3 dB hotter than a quiet slice. One thin bin near 156.781 MHz, about 19 kHz off the channel. Not a voice station. |
| `listen CH16 --seconds 2 --no-speakers` | Dropped 0. ADC −33.4 dBFS. Demod peak 0.65. |

## How to try it

```
cd ~/Documents/vocatron
nix develop
cmake --preset debug && cmake --build --preset debug
./build/debug/vocatron_tests
./build/debug/vocatron tui
./build/debug/vocatron selftest
./build/debug/vocatron list --group marine
./build/debug/vocatron listen POWER_FM --lna 40 --vga 26
./build/debug/vocatron listen LTFM_ATIS --lna 40 --vga 26
./build/debug/vocatron listen PMR1 --lna 40 --vga 26
./build/debug/vocatron listen CH16 --seconds 5 --lna 40 --vga 26 --record samples/ch16.wav
./build/debug/vocatron listen PMR1 --squelch -40 --lna 40 --vga 26
./build/debug/vocatron listen POWER_FM --squelch -20 --lna 40 --vga 26
./build/debug/vocatron demod samples/fm_lna40.iq --mode wfm --out samples/fm_lna40.wav
```

VGA above 26 is rejected. This firmware accepts it and then sends no bytes.
`--amp` is off unless you pass it.

Length was not the limit on LTFM ATIS. It was copied at every whip stop.
The short stop was the quieter one. The screen was the limit: LNA 16 left
the converter at a few counts, and squelch −40 shut the airport radio even
after the gain came up. If the loudest sample hits 127, lower LNA with `-`.
Do not turn the RF amp on.

## Squelch

The gate watches how strong the filtered channel is, in dB below a full-scale
wave. It does not watch the speaker. Empty FM is loud hiss, and AM turns its
own noise up, so those are the wrong clues.

The level is the open point. The gate stays open for 0.3 s after the channel
falls 4 dB below that point, and the audio fades over 10 ms. Mute applies to
the speaker and to `--record` together. The status word is `sql off`, `sql open`,
or `sql shut`.

The channel filter itself makes a full-scale wave look huge, so the meter
divides that gain back out. A half-scale narrow-FM test tone then sits between
−18 and 0 dBFS. Amp off for every row below.

| Source | Mode | Gain | Channel |
|---|---|---|---|
| `samples/pmr1.iq` | nfm | LNA 40, VGA 26 | −54.1 dBFS |
| live PMR1, 2 s, `--squelch -40`, speakers off | nfm | same | −55.3, `sql shut`, demod peak 0.000, dropped 0 |
| `samples/ch16.iq` | nfm | same | −60.3 |
| `samples/ru1_17cm.iq` | nfm | same | −55.4 |
| `samples/ltfm_atis.iq` | am | same | −49.0 |
| `samples/fm_lna40.iq` | wfm | same | −30.4 |
| live POWER_FM, `--squelch -40`, speakers off | wfm | same | −34.1, `sql open`, demod peak 1.444, dropped 0 |
| live POWER_FM, `--squelch -20`, speakers off | wfm | same | −34.2, `sql shut`, demod peak 0.000, dropped 0 |
| `samples/fm_lna16.iq` | wfm | LNA 16, VGA 20 | −53.3 |

`-40` was the right example for those older narrow-FM and AM captures.
Wide FM at LNA 40 / VGA 26 is louder than that on its own noise, so that
same example stays open. `-20` shut it. Speakers stayed off for these checks.

The station screen does not use one gate. At LNA 40 / VGA 20 it opens AM at
−58 (ATIS sat near −50), wide FM at −32 (empty FM sat near −41), and narrow
FM at −48 (empty PMR sat near −60). `[` `]` and `s` hold until the station
name changes.

## Station screen

`vocatron tui` draws the channel list and plays the highlighted station.
It needs a terminal. Up and down move. Enter tunes. `[` and `]` move the
squelch by 2 dB and keep that setting until the station changes. `s` turns
the gate off. `m` mutes the speaker. `-` and `=` step the LNA by 8 dB and
retune. `g` cycles the group. Esc stops the radio. `q` quits.

It opens already listening to LTFM ATIS, LNA 40, VGA 20, amp off, speaker on.
AM starts at squelch −58, wide FM at −32, narrow FM at −48. The gate starts
closed, so an empty channel does not hiss. Changing station puts the gate
back on that mode's level.

An earlier live check at LNA 16 and squelch −40 showed PMR1 near −68 dBFS
with `sql SHUT`, clip 0, dropped 0, underruns 0. That gain sits under the
airport radio. LNA 40 is the start now. The line under the meter says to
raise LNA when a voice is thin. It does not talk about whip length.

## Not done

- `hardware.hackrf.enable` is not in `~/Documents/nixos`. The board opened without it.
- `bw_khz` does not change the filter yet. Every narrow-FM channel uses the ±8 kHz chain.
- No `scan`. The parser is still the hand-written one. CLI11 is named in the spec and is not vendored.
- Nothing is recorded for a future `wireless` group.

## LTBA names in the channel file

Checked against DHMİ AIP `LT_AD_2_LTBA_en.pdf` (page dated 2026, AIRAC) and
`LT_AD_2_LTFM_en.pdf`. The channel file was not changed.

128.200 MHz is Yeşilköy Information, the LTBA voice loop, H24. The file
still calls that channel `LTFM_ATIS`. LTFM's own loops are 128.850 MHz
(departure) and 126.350 MHz (arrival). There is no password and no tone
code on these. They are plain AM.

`LTBA_TWR` is 118.100. The AIP marks that as the clearance / arrival
frequency ("contact 118.100 ten minutes before start up"). The STAR chart
splits tower as 118.100 arrival and 118.375 departure. 118.375 is not in
the file. A D-ATIS text for LTBA on 2026-10-08 told crews to call the
tower on 118.100 for ground and clearance, so `LTBA_GND` (121.700) and
`LTBA_DEL` (121.900) can sit silent while the airport is still open.
LTBA is limited to cargo, maintenance, general aviation, air taxi,
business, and state flights, about 10 movements an hour. Between calls
the tower is supposed to be quiet.

## Next

Scanning a group is the long remaining piece. The station screen is how you
listen. It opens on 128.200, which the file still labels LTFM ATIS. Empty
channels stay silent. A station that climbs past the gate will open. Press
`-` if the clip percent leaves zero. The LTBA / LTFM names above are the
open correction in the channel file.
