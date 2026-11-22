# Ember quick start

Ember is a synth for the R36MAX: warm pads and a vocoder, played from the
handheld's buttons or a USB MIDI keyboard, with a looper and a recorder.
This page gets you going; the [README](README.md) has everything.

## Install or update

From an SSH session on the handheld (`ssh ark@r36max.local`):

```sh
cd /roms/ports && curl -L https://github.com/ruderigo/Ember/releases/latest/download/ember-arkos-arm64.tar.gz | tar xz --no-same-owner
```

Restart EmulationStation, then open **Ports → Ember**. Updating keeps your
settings, presets and recordings. (On an OS with an Apps window, it offers
Ember's updates there.)

## Play

| Button | What it does |
|---|---|
| A / B / X / Y | Chords I, IV, V, vi |
| L1 + A / B / X / Y | Chords ii, iii, vii, I up an octave |
| R1 (hold) | Chords keep sounding |
| START | Looper: record, play, overdub, play |
| SELECT | Tap: undo the last overdub; hold: clear the loop |
| D-pad | ↑/↓ a setting, ←/→ change it |
| L2 / R2 | The pages: PLAY, TONE, VOICE, SPACE, LOOP, INFO |
| Sticks | Left: vowel and filter; right: pitch bend and reverb swell |
| SELECT + START | Quit |

## Sound

- **What:** only what you play. No clicks, no notifications, no start-up
  sound.
- **Where:** USB-C headphones (or any USB sound device) when plugged in,
  otherwise the speaker; it follows you plugging and unplugging. INFO →
  *Output* names it.
- **Volume:** PLAY → *Volume*, and the handheld's volume buttons.
- **Crackles:** set `EMBER_PERIOD=512` in `Ember.sh`.

## The mic (vocoder)

1. Plug in a USB mic or a USB-C headset with a mic.
2. VOICE → *Vocoder* above 0%, *Source* → USB mic.
3. Sing or speak while you play chords.

INFO → *Mic* shows its name and a level that moves when you talk. If it
says **muted?**, its mic is switched off in the headset's own settings
(`alsamixer -c N`, F4, space). If it says **none**, the headset isn't
offering a mic: some USB-C adapters only do with a 4-contact headset plug.
No mic? VOICE → *Source* → vowel voice, a built-in choir.

## Recordings

LOOP → *Record to SD* (→ starts, ← stops) or *Save loop to SD*. The WAVs
go to `/roms/ports/ember/recordings/`. To copy them to your computer:

```sh
scp 'ark@r36max.local:/roms/ports/ember/recordings/*.wav' ~/Music/
```

## If something's wrong

- **OUT is red:** another app has the sound; quit it, Ember retries.
- **The mic:** `ssh ark@r36max.local cat /roms/ports/ember/ember.log`
  shows what it did.
- More: [README → Troubleshooting](README.md#troubleshooting).
