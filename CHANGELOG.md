# Changelog

All notable changes to Ember. The version in `VERSION` is shown on screen
and must match the release tag (`v` + version).

## [0.4.3] - 2026-11-21

### Fixed
- **A USB-C headset's mic is heard on the R36MAX.** The same headset's mic
  worked in FireFly, which records through `arecord`, and its sound
  played in Ember, but Ember heard nothing. The difference was the capture
  buffer: Ember asked for 16 ms (reads every 4 ms), arecord about half a
  second, and the RK3326's USB controller copes badly with USB audio
  capture at such short intervals. Ember now asks for 100 ms, then
  arecord's half second. It still reads 256 samples at a time, so the
  vocoder gains only a period of delay (about 25 ms), not the buffer.
- The vocoder keeps a period and a half of mic sound queued (16 ms at
  least), so a mic that delivers 25 ms at a time doesn't run dry between.

### Added
- `ember.log` says what buffer and period the mic was given.
  `EMBER_MIC_BUFFER_MS` sets the buffer to try first.

## [0.4.2] - 2026-11-21

### Fixed
- **A USB headset's mic no longer flickers** "detected / not detected" on
  the INFO page and the MIC line. Any read error other than an overrun
  was taken for "unplugged": the mic was dropped, found again at once,
  failed again, and so on, while the same headset's playback kept going.
  A read error is now recovered in place; after 8 in a row the mic is
  reopened, with the longer buffer; it's only "gone" when its card is
  gone. It stays on screen meanwhile.
- **Ember.sh no longer stops at a password prompt.** It ran `sudo chmod`
  on the console; where sudo asks for a password (a launch from a dock has
  nowhere to type one) it waited forever. It now uses `sudo -n` and goes
  on without it.

### Added
- `ember.log` notes each mic read error with ALSA's reason, and each
  reopen: the cause of a flicker, readable over ssh.

## [0.4.1] - 2026-11-20

### Fixed
- A USB mic that refuses Ember's short capture buffer now works: Ember
  takes a longer one, as it already did for USB outputs. Before, such a
  mic (some USB-C headset adapters) was retried forever and never heard,
  with nothing on screen to say why.
- When a mic is found but won't open or set up, the INFO page and the
  main screen's MIC line say why (the device and ALSA's error) instead of
  "none" / "not connected".

### Added
- What the mic does goes to `ember/ember.log`, one line per change: the
  device it opened (and whether it took the longer buffer), why it won't
  open, when it hears sound, when it reads only silence, when it's
  unplugged. For when the headset takes the USB port the Wi-Fi was in:
  use Ember, plug the Wi-Fi back, then read the log over ssh.
- The INFO page shows the mic's level (`[######....]`), and after three
  seconds of exact silence, "muted? alsamixer -c N": a USB mic whose
  capture is switched off in its own mixer is connected but hears nothing.

## [0.4.0] - 2026-11-19

### Added
- Sound follows a USB sound device. With USB headphones, a USB-C audio
  adapter or a USB DAC plugged in, Ember plays through it, and it moves
  there or back to the built-in sound within a couple of seconds when the
  device is plugged in or unplugged while Ember is running. The INFO page
  shows the output in use and the device's name.

### Changed
- `EMBER_OUT` now means a fixed output: when it is set, Ember stays on that
  device and does not follow USB devices. Use `EMBER_OUT=plughw:0,0` to keep
  the built-in sound even with a USB sound device plugged in.

### Fixed
- No sound on a handheld whose speaker is not working while games played
  fine through USB headphones: Ember always used the built-in sound card.

## [0.3.0] - 2026-10-10

### Added
- 10 new factory presets (16 in total): Juno Dawn, Brass Haze, Felt Keys,
  Midnight Choir, Whisper Pad, Mic Choir, Cassette Sunset, Deep Sub Drone,
  Shimmer Wash and Soft Keys.
- 8 user preset slots on the PLAY page (**Save to**, **Save preset**), with
  a confirm step before overwriting. Stored in `user_presets.cfg`, where
  slots can be renamed.
- A `*` after the preset name when the sound has been changed.
- Presets now also store the vowel, chord type and bass note.

### Changed
- The version shown on screen comes from the `VERSION` file.

## [0.2.1] - 2026-10-10

### Fixed
- SELECT and START did nothing on handhelds whose controller mapping leaves
  them out. They are now always read from the raw button numbers.

## [0.2.0] - 2026-10-10

### Added
- Looper: START records, plays and overdubs; tap SELECT to undo or redo an
  overdub, hold SELECT to clear. Loops up to 64 seconds, captured after the
  reverb, with a crossfaded seam.
- LOOP page with loop level and **Overdub keep** (how much of the old loop
  survives each pass).
- Recording everything you hear to the SD card, and saving the current loop,
  as 48 kHz WAV files in `recordings/`.

## [0.1.0] - 2026-10-10

### Added
- Pad synth: 8 voices of detuned saws and a sub oscillator with analog
  drift, ladder filter, envelopes and tape saturation.
- 16-band vocoder fed by a USB mic (with clock drift correction) or a
  built-in vowel voice.
- Stereo chorus and 8-line FDN reverb.
- Chords from the face buttons in any key and scale, hold, stick control.
- USB MIDI keyboards with hot-plug, sustain, pitch bend and common CCs.
- 720x720 screen with pages for play, tone, voice, space and device info.
- 6 factory presets, settings saved automatically.

[0.4.3]: https://github.com/ruderigo/Ember/releases/tag/v0.4.3
[0.4.2]: https://github.com/ruderigo/Ember/releases/tag/v0.4.2
[0.4.1]: https://github.com/ruderigo/Ember/compare/v0.4.0...v0.4.2
[0.4.0]: https://github.com/ruderigo/Ember/releases/tag/v0.4.0
[0.3.0]: https://github.com/ruderigo/Ember/releases/tag/v0.3.0
[0.2.1]: https://github.com/ruderigo/Ember/commits/main
[0.2.0]: https://github.com/ruderigo/Ember/commits/main
[0.1.0]: https://github.com/ruderigo/Ember/commits/main
