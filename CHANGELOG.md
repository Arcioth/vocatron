# CHANGELOG — vocatron

## 2026-10-11

- Renamed from radiovoc to vocatron (folder, CMake targets, namespace,
  docs). Published at https://github.com/Arcioth/vocatron with a README.
- Speaker gain per mode: AM 0.05, narrow FM 1.0, wide FM 0.25. Narrow FM
  voice was about 26 dB too quiet. New `tests/level_test.cpp` checks that a
  voice-level tone lands near −22 dBFS RMS in every mode and hiss never clips.
- The station screen changes station and LNA in place. The radio and the
  speakers stay open. A user-chosen stop no longer reports dropped bytes as
  a broken receiver.
- RF: the USB-C adapter chain raised the airband floor about 5 dB. With a
  direct cable at spot 1, ATIS reads −35.5 dBFS. FM is still weak on the
  25 cm whip. Details in HANDOFF.md.
- First live wide FM: music on 103.0 and KRAL FM 92.0.
- Station screen rebuilt with FTXUI: mouse clicks, group tabs, buttons for
  gain, VGA, RF amp (off at start, turns itself off on clipping), squelch,
  mute, stop/play. Starts at LNA 40 / VGA 26. Gate starts at −52 dBFS for
  AM, −48 for narrow FM, off for broadcast FM.
- Audio gaps fixed: playback keeps a 150 ms cushion and refills it after
  running dry, instead of 50 ms with no refill.
- Wide FM: 15 kHz audio filter (drops the 19 kHz pilot), tighter ±80 kHz
  channel filter, and a clarity gauge that drives a weak-signal treble cut.
- RDS decoder: station names and codes. TRT FM decodes as "TRT FM".
- `vocatron scan` and a Scan FM button: about 60 stations found in about a
  minute, with clarity and RDS names. Found tab, Save button, Saved tab,
  and `channels/saved.toml` for the stations you confirm you can hear.
- Airband list corrected against SkyVector/learn-atc (towers, ground,
  delivery, ATIS). 128.200 is LTBA_ATIS. AM channel filter is two steps and
  sharp at the next 8.33 kHz channel.
- Band search (`vocatron search <band>`, Search in the screen) for airband,
  marine, 2 m, 70 cm, PMR446, CB and shortwave: watches for transmissions
  and lists channels as talk ×N or steady, with interference combs hidden.
- Scan list: hop through a tab's stations and stop on the one that talks.
- Naming: Save asks for a name (RDS name suggested), saved stations can be
  renamed or forgotten, and your names show in every tab. UTF-8 names
  (Turkish letters) work.
- New README with the logo (`docs/vocatron.png`).

## 2026-10-09

- LTFM ATIS on 128.200 MHz was heard with a hand around the antenna at every
  whip length. The shortest stop was quieter. Fresh captures, amp off, VGA 20:
  LNA 16 is flat hiss (channel −69 dBFS, loudest sample 5). LNA 40 holds the
  carrier 27 dB over a narrow bin and the demod has speech rhythm (channel
  −50.5, level variation 0.58). 100.000 MHz at the same gain is not a wide
  FM station (channel −41.4).
- AM keeps about ±3.5 kHz, then a 2.7 kHz voice filter. The volume control
  is 0.3 s, so gaps between words are not lifted to the voice. The first
  50 ms stays silent.
- `vocatron tui` opens on LTFM ATIS at LNA 40 / VGA 20, amp off. The gate is
  −58 for AM, −32 for wide FM, and −48 for narrow FM. `[` `]` and `s` hold
  until the station changes. The length hint is gone from the screen and
  from `listen`. Debug tests: 17 cases, 10159 assertions.
- Channel names were not edited. DHMİ AIP lists 128.200 as Yeşilköy / LTBA
  ATIS, and LTFM ATIS as 128.850 (departure) and 126.350 (arrival).
  `LTBA_TWR` 118.100 is the arrival/clearance frequency. Departure tower
  is 118.375, which is not in the file.

## 2026-10-08

- Dev shell, debug/release CMake presets, and `vocatron info`. With the
  HackRF plugged in this prints board id 4, firmware `n_260928`, API 1.13,
  and the known serial and part id.
- `vocatron capture` records receive-only int8 I/Q and prints an averaged
  FFT: DC spike, narrow bins, and how much hotter the channel is than the
  quietest equal-width slice. Amp defaults off. VGA above 26 is rejected.
  A zero-byte stream is an error.
- `vocatron selftest` checks the FFT on a generated tone (99.825 MHz, −6.0 dBFS).
- Live 100.0 MHz captures with the whip fully extended did not show an FM
  broadcast. Default gain left the converter at a few counts. LNA 40 / VGA 26
  and a later amp-on pass still had no wide station. Files:
  `samples/fm_lna16.iq`, `samples/fm_lna40.iq`, `samples/fm_amp.iq`.
- The whip's full length is about 25 cm. At the same high gain, 940 MHz
  filled and clipped while 100 MHz and 439.150 MHz stayed quiet.
  `samples/ru1.iq` is the 439.150 capture. The 940 MHz files were deleted.
- Whip collapsed to about 17 cm and 439.150 captured again
  (`samples/ru1_17cm.iq`). The narrow carriers near 440.000 and 439.200
  rose about 8 dB. The repeater channel itself stayed empty.
- Sample queue between the USB thread and a consumer thread. `ringtest`
  held 3 s at 2.4 MS/s with nothing dropped. Unit tests cover wrap,
  a full queue, and a paced 2 s run.
- Offline wide FM (`demod --mode wfm`). A generated 1 kHz tone comes back
  out of a 48 kHz WAV. Real 100 MHz captures demodulate to hiss.
- `listen` plays 48 kHz audio through PulseAudio, with a one-line status
  of ADC level, clip percent, dropped bytes, and underruns. Named FM
  channels work. `POWER_FM` for 1.2 s dropped nothing.
- Offline and live AM (`demod --mode am`, `listen LTFM_ATIS`). A generated
  1 kHz AM tone comes back. A live 128.200 MHz check with the whip at
  about 17 cm did not show a voice-wide station. Narrow FM is refused.
- Offline and live narrow FM (`demod --mode nfm`, `listen CH16`, `listen PMR1`).
  A generated 1 kHz tone at 3 kHz deviation comes back, and a steady carrier
  stays quiet. Live 446.006 MHz and 156.800 MHz, whip about 17 cm, LNA 40,
  VGA 26, amp off, were not voice stations. Speakers stayed off. Dropped 0.
- Named stations moved to `channels/istanbul.toml` (toml++ 3.4.0).
  `vocatron list` and `list --group marine` print them. `listen --record`
  writes the same WAV as `--out`.
- `--squelch LEVEL` mutes the speaker and the recording from the filtered
  channel level, not from the hiss. Off unless passed. Opens at the level,
  holds 0.3 s after a 4 dB drop, fades in 10 ms. The status line shows
  the frequency, `ch` dBFS, and `sql off|open|shut`. Empty PMR1 at LNA 40 / VGA 26 read
  −55 dBFS and stayed shut at −40, with demod peak 0. Wide FM at the same
  gain read about −34 and needed about −20 to shut. Speakers stayed off.
  Debug tests: 14 cases, 10129 assertions.
- `vocatron tui` is the station screen. It listens to PMR1 at LNA 16 / VGA 20
  with the speaker on and squelch at −40. Arrow keys and enter change the
  station. `[` `]` move the gate, `s` toggles it, `m` mutes, `g` filters the
  group, `-` `=` step the LNA. A live run showed PMR1 near −68 dBFS with
  `sql SHUT`, nothing dropped, and no underruns. Debug tests: 17 cases,
  10159 assertions.
