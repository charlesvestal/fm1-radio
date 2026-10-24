# Favorites and a small synth: design

2026-10-24. Two features for Radio, on pages of their own.

## Favorites (SAVE on Home)

**Page.** A list. Its first row is "+ Add *current station*": dimmed, and SEL does nothing, when no station is
tuned or the station is a favorite already. Below it the favorites, in the order they were added, each as its
title and place.

**Controls.** Turning SELECT moves the highlight. SEL on the first row adds the current station at the end of
the list; SEL on a favorite tunes it, plays it and goes back to Home. REC deletes the highlighted favorite at once
(no confirmation). HOME goes back to Home. With 16 favorites the first row reads "Favorites full (16)".

**What is kept.** Up to 16 favorites, each: country index (u16), place name (32 bytes), station id (12), station
title (32): 78 bytes, 1248 in all, with a magic and a count.

**Where.** Their own store object, OBJ_FAV, an A/B pair of 4 KB sectors at 0xD6000 (app.h APP_STORE_MAP). The
places table's area ends there instead of at 0xD8000 (places.c RGP_END, rg.c RG_MAX): it keeps 258,048 bytes for a
table measured at 129,895 (`radio_ctl.py rginfo`, 2026-10-24).

**RAM.** None kept. The list is loaded into a 1.3 KB heap buffer when the page opens and freed when it closes; an
add or a delete saves the whole object at once.

**Tuning a favorite.** As the start does with the last station (radio_ui_restore): its country, its place found by
name (rg_place_index), then the station by id; the station plays before the place's list is fetched. A favorite
whose place is no longer in the table says so ("place gone") and stays in the list.

## Synth (ARP on Home)

**Page.** ARP on Home opens Synth. The 27 keys (F3..G5) play notes, mixed over whatever the radio plays (or alone
when it is stopped). OCT- / OCT+ shift by an octave, -2..+2. ARP or HOME goes back to Home, where the keys are the
punch-in FX again; notes held when leaving are released.

**Sound.** 4 voices; a new note takes a free voice, else the oldest. Per voice one oscillator whose wave morphs
sine -> triangle -> saw -> square (a crossfade between neighbours; saw and square band-limited with polyBLEP) and an
attack / release envelope (full sustain while the key is held). One 2-pole low-pass (state-variable, fixed light
resonance) on the synth's mix.

**Controls.**

| Control | Sets | Range |
| --- | --- | --- |
| Knob 1 | wave | sine .. triangle .. saw .. square |
| Knob 2 | filter cutoff | 80 Hz .. 12 kHz (exponential) |
| Knob 3 | attack | 1 ms .. 2 s |
| Knob 4 | release | 5 ms .. 3 s |
| SELECT (turned) | synth level against the radio | 0 .. 100 % |

**Mix.** The synth's mix is added to the radio's before MASTER (whose squared taper then covers both); its level
by SELECT. A voice at full is -12 dBFS, so 4 voices sum to 0 dBFS at most before the filter; the radio plus the
synth (and the filter's resonance) can still pass full scale, so the sum is clamped at full scale before MASTER.

**Kept.** The 5 settings and the octave, in the last station's object (OBJ_RADIO, rset): a few bytes.

**Screen.** The four knob values with the wave's shape drawn, the level, the octave, and a small keyboard with the
held notes lit (as the password page draws one: ui_piano).

**Where.** A new file, app/synth.c (the voices, the filter, the render, note on / off, its settings); app.c's
input and pages call it; radio_render adds its output. Part of the app's unity build, like the others.

**Cost.** In the audio interrupt: 4 oscillators and one filter a sample, single-precision float, no libm
(fastmath.h). RAM under 200 bytes; no heap.

## Testing

- Host simulator: the synth rendered offline, checked for level (no sample past full scale with 4 voices and
  level at 100 %), each knob's range at both ends, note on / off and voice stealing; favorites added, deleted, full,
  and kept across a restart through the simulator's store.
- FM-1: chords held over an AAC station while the decoder runs (no underruns added, the CPU figure from
  `radio_ctl.py dump`), the heap unchanged by the synth; favorites added, tuned, deleted, kept after a power cycle.

## Not in this

No arpeggiator, no filter envelope, no presets, no MIDI in or out for the synth.
