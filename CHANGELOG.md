# Changelog

All notable changes to Ember. The version in `VERSION` is shown on screen
and must match the release tag (`v` + version).

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

[0.3.0]: https://github.com/ruderigo/Ember/releases/tag/v0.3.0
[0.2.1]: https://github.com/ruderigo/Ember/commits/main
[0.2.0]: https://github.com/ruderigo/Ember/commits/main
[0.1.0]: https://github.com/ruderigo/Ember/commits/main
