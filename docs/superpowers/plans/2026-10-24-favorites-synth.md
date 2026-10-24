# Favorites and a small synth: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two new pages in Radio: Favorites on SAVE (add the current station, tune one, delete with REC) and a
4-voice synth on ARP (keys play over the radio; knobs wave / filter / attack / release; SELECT level; OCT-/+).

**Architecture:** Two new files in Radio's unity build: `app/synth.c` (the engine: voices, morphing oscillator,
envelope, one low-pass, rendered a sample at a time from `radio_render` in the audio interrupt) and `app/fav.c`
(the favorites' list operations and the page's state). `app/app.c` gets the pages, input, lights, drawing, the
mix and the store; `app/radio_ui.c` gets `fav_tune`. Favorites live in a new store object (OBJ_FAV, 0xD6000);
the places area shrinks to end there. The synth's settings ride in the last station's object (rset).

**Tech Stack:** C (C99, single-precision float, no libm: `base/include/fastmath.h`), fm1-base platform
(`base/include/plat.h`), the fm1-base host simulator (`app/build/host/radio_host SCRIPT`), plain `cc` unit tests.

**User decisions (already made):**
- Favorites: SAVE on Home opens a list; its top row "+ Add *current station*" adds it; SELECT scrolls, SEL plays a favorite, REC deletes at once; HOME back.
- Synth: ARP opens its own page; keys play over the radio; 4 voices; knobs = wave morph, filter, attack, release; OCT-/OCT+ octaves; SELECT = synth volume.
- Favorites in their own 8 KB store slot at 0xD6000 (places table measured 129,895 bytes of 266,240).

Spec: `docs/superpowers/specs/2026-10-24-favorites-synth-design.md`.

## Conventions for every task

- Work in the Radio repo root (`fm1-radio/`), on `main`. Build: `PYTHON=/opt/homebrew/bin/python3.14 ./build.sh`
  (device), `./build.sh --host`, `./build.sh --web`; all three must succeed before a commit.
- **The main checkout's `base/` holds an uncommitted diagnostic tree that does not link.** Build and test in the
  worktree `../fm1-radio-mulfix` instead (`git -C ../fm1-radio-mulfix merge --ff-only main` after each commit,
  its `base/` on the commit Radio pins, clean: `git -C ../fm1-radio-mulfix/base status --short` empty).
- Comments in the codebase's style: short, plain, what and why; no "TODO".
- Commit messages end with:
  ```
  Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01UGtNzPdY7XxqMyvqRESveN
  ```
- Never `git add base` from the main checkout (it would pin the diagnostic tree).

## File map

| File | Responsibility |
| --- | --- |
| `app/synth.c` (new) | the engine: settings type, voices, oscillator, envelope, filter, `synth_next()`, the knob value -> Hz / ms maps |
| `app/fav.c` (new) | the favorites' list type and operations (add, delete, find, valid), the page's state (`fav`, `fav_sel`) |
| `app/app.c` | includes; rset gains the synth settings; pages PG_SYNTH / PG_FAV: input, lights, drawing; `app_expect` |
| `app/app.h` | OBJ_FAV in the store map; `APP_HOST_EXPECT` |
| `app/radio.c` | `radio_render` adds the synth and clamps |
| `app/radio_ui.c` | `fav_tune()`; help rows; the dump's favorites line |
| `app/places.c`, `app/rg.c` | the places area ends at 0xD6000 |
| `tests/run_tests.sh` (new) | unit tests, then the host scripts |
| `tests/synth_test.c`, `tests/fav_test.c` (new) | `cc` unit tests of the two new files alone |
| `tests/synth.host`, `tests/fav.host` (new) | host-simulator scripts |

---

### Task 1: The synth engine, alone, with its unit test

**Goal:** `app/synth.c` renders 4 voices with a morphing oscillator, attack/release and a low-pass, proven by a `cc` unit test.

**Files:**
- Create: `app/synth.c`
- Create: `tests/synth_test.c`
- Create: `tests/run_tests.sh`

**Acceptance Criteria:**
- [ ] No note held: `synth_next()` returns exactly 0.0
- [ ] Note 69 (sine, filter open): 438..442 rising zero crossings in one second
- [ ] 4 notes, square, level 127, filter open: peak between 0.2 and 1.3
- [ ] A fifth note steals the oldest voice (the first note is no longer playing; 4 voices on)
- [ ] Attack 0 reaches > 0.9 envelope within 5 ms; attack 127 is < 0.1 after 100 ms
- [ ] Release 0: all voices off within 10 ms of note off
- [ ] Cutoff 0 vs 127 on note 81 square: RMS ratio < 0.3

**Verify:** `sh tests/run_tests.sh` → `synth_test: all passed`

**Steps:**

- [ ] **Step 1: Write the failing test** `tests/synth_test.c`:

```c
/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* app/synth.c alone: cc -O1 -Ibase/include -o build/synth_test tests/synth_test.c && ./build/synth_test */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../app/synth.c"

static int bad;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "  ok" : "FAIL", what);
    bad += !ok;
}
static float fabs_(float x) { return x < 0 ? -x : x; }
static void reset(uint8_t wave, uint8_t cutoff, uint8_t attack, uint8_t release, uint8_t level)
{
    synth_set_t s = {wave, cutoff, attack, release, level, 0};
    synth_init();
    syn = s;
    synth_update();
}
static int voices_on(void)
{
    int i, n = 0;
    for (i = 0; i < SYN_VOICES; i++)
        n += syv[i].on;
    return n;
}

int main(void)
{
    int i, k, cross = 0;
    float prev = 0, peak = 0, rms_open = 0, rms_shut = 0;

    reset(0, 127, 0, 0, 127);
    for (i = 0, k = 1; i < 1000; i++)
        k &= synth_next() == 0.0f;
    check(k, "silent with no note");

    reset(0, 127, 0, 0, 127);
    synth_note_on(69);
    for (i = 0; i < 44100; i++) {
        float s = synth_next();
        cross += prev < 0 && s >= 0;
        prev = s;
    }
    check(cross >= 438 && cross <= 442, "note 69 at 440 Hz");

    reset(127, 127, 0, 0, 127);
    synth_note_on(60), synth_note_on(64), synth_note_on(67), synth_note_on(72);
    for (i = 0; i < 44100; i++) {
        float a = fabs_(synth_next());
        if (a > peak)
            peak = a;
    }
    check(peak > 0.2f && peak < 1.3f, "4 square voices: peak 0.2 .. 1.3");

    synth_note_on(76);
    for (i = 0, k = 1; i < SYN_VOICES; i++)
        k &= !(syv[i].on && syv[i].gate && syv[i].note == 60);
    check(k && voices_on() == 4, "a fifth note steals the oldest");

    reset(0, 127, 0, 0, 127);
    synth_note_on(60);
    for (i = 0; i < 220; i++)
        synth_next();
    check(syv[0].env > 0.9f, "attack 0: full within 5 ms");

    reset(0, 127, 127, 0, 127);
    synth_note_on(60);
    for (i = 0; i < 4410; i++)
        synth_next();
    check(syv[0].env < 0.1f, "attack 127: still low after 100 ms");

    reset(0, 127, 0, 0, 127);
    synth_note_on(60);
    for (i = 0; i < 441; i++)
        synth_next();
    synth_note_off(60);
    for (i = 0; i < 441; i++)
        synth_next();
    check(voices_on() == 0, "release 0: off within 10 ms");

    reset(127, 127, 0, 0, 127);
    synth_note_on(81);
    for (i = 0; i < 4410; i++) {
        float s = synth_next();
        rms_open += s * s;
    }
    reset(127, 0, 0, 0, 127);
    synth_note_on(81);
    for (i = 0; i < 4410; i++) {
        float s = synth_next();
        rms_shut += s * s;
    }
    check(rms_shut < rms_open * 0.09f, "cutoff 0: RMS under 0.3 of open");

    printf(bad ? "synth_test: %d failed\n" : "synth_test: all passed\n", bad);
    return bad != 0;
}
```

- [ ] **Step 2: Create the runner** `tests/run_tests.sh` (unit tests only for now; Task 2 adds the host scripts):

```sh
#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Radio's tests: the synth and the favorites' list alone (cc), then the app in fm1-base's host simulator.
#   sh tests/run_tests.sh   (from the repo root; PYTHON as for ./build.sh)
set -e
cd "$(dirname "$0")/.."
mkdir -p app/build/tests
for t in synth fav; do
    [ -f tests/${t}_test.c ] || continue
    cc -O1 -Wall -Ibase/include -o app/build/tests/${t}_test tests/${t}_test.c
    app/build/tests/${t}_test
done
if ls tests/*.host >/dev/null 2>&1; then
    ./build.sh --host >/dev/null
    for s in tests/*.host; do
        echo "== $s"
        app/build/host/radio_host "$s" app/build/tests
    done
fi
echo "all tests passed"
```

- [ ] **Step 3: Run it to see it fail**

Run: `sh tests/run_tests.sh`
Expected: cc error `../app/synth.c: No such file or directory`.

- [ ] **Step 4: Write** `app/synth.c`:

```c
/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* A small synth for the keys (ARP's page): 4 voices, each one oscillator whose wave morphs sine -> triangle -> saw
 * -> square (a crossfade between neighbours; saw and square band-limited, polyBLEP) and an attack / release
 * envelope (full sustain while held); one 2-pole low-pass (state-variable, trapezoidal: stable at any cutoff) on
 * their mix. synth_next() runs in the audio interrupt (radio_render), a sample at a time; the rest from the main
 * loop. Settings 0..127. Part of the app's unity build; tests/synth_test.c builds it alone. */
#include <stdint.h>
#include "fastmath.h"

#define SYN_VOICES 4
#define SYN_SR 44100.0f
typedef struct { uint8_t wave, cutoff, attack, release, level; int8_t octave; } synth_set_t;
static const synth_set_t SYN_DEFAULT = {42, 96, 4, 48, 90, 0};
static synth_set_t syn = {42, 96, 4, 48, 90, 0};

typedef struct {
    float ph, inc, env;                           /* phase and its step, in turns; the envelope 0..1 */
    uint8_t note, gate, on;                       /* gate: held; on: sounding (until the release ends) */
    uint32_t age;                                 /* when it was started: the oldest is stolen */
} syn_voice_t;
static syn_voice_t syv[SYN_VOICES];
static uint32_t syn_age;
static float syn_g, syn_a1, syn_a2, syn_a3, syn_ic1, syn_ic2;   /* the filter: coefficients, state */
static float syn_att, syn_rel, syn_gain;          /* envelope steps a sample; the mix's gain */

/* the knobs' values, exponential: also what the page shows */
static float synth_cutoff_hz(void) { return 80.0f * fm_exp2f(7.2288f * (float)syn.cutoff / 127.0f); }   /* ..12 kHz */
static float synth_attack_ms(void) { return fm_exp2f(10.966f * (float)syn.attack / 127.0f); }          /* 1..2000 */
static float synth_release_ms(void) { return 5.0f * fm_exp2f(9.229f * (float)syn.release / 127.0f); }   /* 5..3000 */

static void synth_update(void)                    /* the coefficients from syn (main loop) */
{
    const float k = 1.0f;                         /* damping 1/Q: a little resonance */
    float g = fm_tanf(3.14159265f * fm_minf(synth_cutoff_hz(), 0.45f * SYN_SR) / SYN_SR);
    float a1 = 1.0f / (1.0f + g * (g + k));
    syn_a1 = a1;
    syn_a2 = g * a1;
    syn_a3 = g * g * a1;
    syn_g = g;
    syn_att = 1000.0f / (synth_attack_ms() * SYN_SR);
    syn_rel = 1000.0f / (synth_release_ms() * SYN_SR);
    syn_gain = 0.25f * (float)syn.level / 127.0f; /* a voice at full: -12 dBFS */
}

static void synth_init(void)
{
    uint32_t i;
    for (i = 0; i < SYN_VOICES; i++)
        syv[i].on = syv[i].gate = 0, syv[i].env = 0.0f, syv[i].ph = 0.0f;
    syn_ic1 = syn_ic2 = 0.0f;
    synth_update();
}

static void synth_note_on(int note)
{
    uint32_t i, v = 0;
    for (i = 0; i < SYN_VOICES; i++)              /* the same note again, or a free voice, or the oldest */
        if (syv[i].on && syv[i].note == note) {
            v = i;
            goto take;
        }
    for (i = 0; i < SYN_VOICES; i++)
        if (!syv[i].on) {
            v = i;
            goto take;
        }
    for (i = 1; i < SYN_VOICES; i++)
        if (syv[i].age < syv[v].age)
            v = i;
take:
    syv[v].note = (uint8_t)note;
    syv[v].inc = 440.0f * fm_exp2f((float)(note - 69) / 12.0f) / SYN_SR;
    syv[v].age = ++syn_age;
    syv[v].gate = 1;
    syv[v].on = 1;                                /* last: the interrupt sees a whole voice */
}

static void synth_note_off(int note)
{
    uint32_t i;
    for (i = 0; i < SYN_VOICES; i++)
        if (syv[i].gate && syv[i].note == note)
            syv[i].gate = 0;
}

static void synth_all_off(void)
{
    uint32_t i;
    for (i = 0; i < SYN_VOICES; i++)
        syv[i].gate = 0;
}

static float syn_blep(float t, float dt)          /* polyBLEP: the step's correction near a discontinuity */
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static float syn_shape(int s, float ph, float dt) /* 0 sine, 1 triangle, 2 saw, 3 square; ph in turns */
{
    switch (s) {
    case 0: return fm_sin_turns(ph);
    case 1: return 4.0f * fm_fabsf(ph - 0.5f) - 1.0f;
    case 2: return 2.0f * ph - 1.0f - syn_blep(ph, dt);
    default: {
        float p2 = ph + 0.5f;
        if (p2 >= 1.0f)
            p2 -= 1.0f;
        return (ph < 0.5f ? 1.0f : -1.0f) + syn_blep(ph, dt) - syn_blep(p2, dt);
    }
    }
}

static float synth_next(void)                     /* a sample of the mix, after the filter (the audio interrupt) */
{
    float m = (float)syn.wave * (3.0f / 127.0f), t, sum = 0.0f, v1, v2, v3;
    int s = (int)m, any = 0;
    uint32_t i;
    if (s > 2)
        s = 2;
    t = m - (float)s;
    for (i = 0; i < SYN_VOICES; i++) {
        syn_voice_t *v = &syv[i];
        float o;
        if (!v->on)
            continue;
        if (v->gate) {
            v->env += syn_att;
            if (v->env > 1.0f)
                v->env = 1.0f;
        } else {
            v->env -= syn_rel;
            if (v->env <= 0.0f) {
                v->env = 0.0f;
                v->on = 0;
                continue;
            }
        }
        o = syn_shape(s, v->ph, v->inc) * (1.0f - t) + syn_shape(s + 1, v->ph, v->inc) * t;
        v->ph += v->inc;
        if (v->ph >= 1.0f)
            v->ph -= 1.0f;
        sum += o * v->env;
        any = 1;
    }
    if (!any)
        return 0.0f;
    sum *= syn_gain;
    v3 = sum - syn_ic2;                           /* the low-pass (Simper's trapezoidal SVF) */
    v1 = syn_a1 * syn_ic1 + syn_a2 * v3;
    v2 = syn_ic2 + syn_a2 * syn_ic1 + syn_a3 * v3;
    syn_ic1 = 2.0f * v1 - syn_ic1;
    syn_ic2 = 2.0f * v2 - syn_ic2;
    return v2;
}
```

Note: the test's "silent with no note" requires `synth_next()` to return exactly 0.0 when no voice is on, which
the `if (!any) return 0.0f;` line guarantees (the filter's state is left as it is).

- [ ] **Step 5: Run the test to see it pass**

Run: `sh tests/run_tests.sh`
Expected: eight `  ok` lines, `synth_test: all passed`, `all tests passed`.
If "note 69 at 440 Hz" fails by a few crossings, check `inc` (turns per sample) and the phase wrap; do not widen
the bounds.

- [ ] **Step 6: Commit**

```bash
git add app/synth.c tests/synth_test.c tests/run_tests.sh
git commit -m "Synth engine (app/synth.c): 4 voices, a morphing wave, attack / release, one low-pass; its unit test

Not yet in the app: the page and the mix come next.

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UGtNzPdY7XxqMyvqRESveN"
```

---

### Task 2: The Synth page: keys, knobs, the mix, kept settings

**Goal:** ARP on Home opens the Synth page; keys play over the radio, knobs and SELECT set the sound, OCT shifts, settings survive a reboot; proven in the host simulator.

**Files:**
- Modify: `app/app.c` (includes; rset; page enum; input; lights; ui_draw; ui_piano; new ui_synth; app_expect)
- Modify: `app/app.h` (`#define APP_HOST_EXPECT`)
- Modify: `app/radio.c` (radio_render: add the synth, clamp)
- Modify: `app/radio_ui.c` (help rows)
- Create: `tests/synth.host`

**Acceptance Criteria:**
- [ ] Host: `tap ARP` from Home → `expect page 6`; a held key → `expect syn_voices 1` and `peak_db_min -30`
- [ ] Host: nothing held → `peak_db_max -90`
- [ ] Host: 4 keys, level and cutoff at the top, MASTER 4096 → `peak_db_max 0.1` (clamped)
- [ ] Host: `spin K1 10` → `syn_wave 62` (42 + 2 a detent); `tap OCT+` ×3 → `syn_octave 2`
- [ ] Host: `tap HOME` → page 0, all notes released; `reboot` → `syn_wave 62`, `syn_octave 2` kept
- [ ] An older saved station (rset without synth settings) still loads: its station kept, synth at defaults
- [ ] Device, host and web builds succeed

**Verify:** `sh tests/run_tests.sh` → `== tests/synth.host` with every line `  ok`, then `all tests passed`

**Steps:**

- [ ] **Step 1: Write the failing host script** `tests/synth.host` (pages: PG_HOME 0 … PG_SYNTH 6):

```
# the Synth page: ARP opens it, keys sound over the radio (none on the host: no network), knobs set, kept
wait 500
expect page 0
master 4096
tap ARP
wait 50
expect page 6
peakreset
wait 200
expect peak_db_max -90
key 12 down
wait 300
expect syn_voices 1
expect peak_db_min -30
key 12 up
wait 1500
expect syn_voices 0
spin K1 10
wait 50
expect syn_wave 62
spin K2 20
spin SELECT 30
wait 50
expect syn_cutoff 127
expect syn_level 127
key 0 down
key 4 down
key 7 down
key 12 down
peakreset
wait 500
expect syn_voices 4
expect peak_db_max 0.1
key 0 up
key 4 up
key 7 up
key 12 up
tap OCT+
tap OCT+
tap OCT+
wait 50
expect syn_octave 2
key 3 down
wait 100
tap HOME
wait 1500
expect page 0
expect syn_voices 0
key 3 up
reboot
wait 500
expect syn_wave 62
expect syn_octave 2
expect syn_level 127
```

- [ ] **Step 2: Run it to see it fail**

Run: `sh tests/run_tests.sh`
Expected: the host build succeeds, then `FAIL unknown expect page` (Radio has no `app_expect` yet) and a non-zero exit.

- [ ] **Step 3: `app/app.h`** — after the `APP_STORE_MAP` line add:

```c
#define APP_HOST_EXPECT                  /* the host simulator's "expect" asks app_expect (app.c) first */
int app_expect(const char *what, int *got);
```

- [ ] **Step 4: `app/app.c` — include the engine and keep its settings with the last station.** Immediately
before the line `/* ---- the station of the last session (OBJ_RADIO) */` insert:

```c
#include "synth.c"
```

Replace the rset block (struct, `rset_load`) with:

```c
/* ---- the station of the last session (OBJ_RADIO), and the synth's settings (saved before them: older saves
 * end at st_title, and load with the synth at its defaults) */
#define RSET_MAGIC 0x52414431u                    /* "RAD1" */
static struct {
    uint32_t magic;
    uint8_t playing, rsv;
    uint16_t country;
    char place[32], st_id[12], st_title[32];
    synth_set_t syn;
} rset;
static void rset_load(void)
{
    int n = plat_store_load(OBJ_RADIO, &rset, sizeof rset);
    int old = (int)((char *)&rset.syn - (char *)&rset);
    if (n < old || rset.magic != RSET_MAGIC) {
        memset(&rset, 0, sizeof rset);
        rset.magic = RSET_MAGIC;
        n = 0;
    }
    if (n < (int)sizeof rset)
        rset.syn = SYN_DEFAULT;
    syn = rset.syn;
    synth_init();
}
```

(`rset_save` stays as it is: it writes the whole struct, synth settings included.)

- [ ] **Step 5: `app/app.c` — the page.** Change the page enum to:

```c
enum { PG_HOME, PG_BROWSE, PG_WIFI, PG_NETS, PG_PASS, PG_HELP, PG_SYNTH, PG_FAV };
```

In `ui_piano`, draw no letters when `layer < 0`. Replace its two label lines:

```c
            char t[3] = {pw_char(k, layer), 0, 0};
```
with
```c
            char t[3] = {layer < 0 ? 0 : pw_char(k, layer), 0, 0};
```
and
```c
            if (!t[0])
                t[0] = '<';
```
with
```c
            if (!t[0] && layer >= 0)
                t[0] = '<';
```
and
```c
            char t[2] = {pw_char(k, layer), 0};
```
with
```c
            char t[2] = {layer < 0 ? 0 : pw_char(k, layer), 0};
```

After `ui_pass` (before `ui_draw`) add the page's state and drawing:

```c
/* ---- the Synth page (ARP): the keys play app/synth.c over the radio */
static uint32_t syn_keys;                         /* held, as last seen */
static int8_t syn_key_note[NKEYS];                /* the note each held key started (an octave change between
                                                   * its down and up must not leave it sounding) */
static uint8_t syn_changed;                       /* settings changed since the page opened: saved on leaving */

static void ui_synth_wave(int slot, uint32_t y)   /* the wave's shape, and the level */
{
    char key[80], lv[24];
    int i;
    snprintf(lv, sizeof lv, "level %u%%", (unsigned)(syn.level * 100u / 127u));
    snprintf(key, sizeof key, "W%u|%s|%d", syn.wave, lv, radio_st == RS_PLAYING);
    if (w_eq(ui_key[slot], key))
        return;
    memcpy(ui_key[slot], key, sizeof key);
    cv_begin(240, 24, K_BG);
    {
        float m = (float)syn.wave * (3.0f / 127.0f), t;
        int s = (int)m;
        if (s > 2)
            s = 2;
        t = m - (float)s;
        for (i = 0; i < 96; i++) {
            float ph = (float)i / 96.0f;
            float v = syn_shape(s, ph, 0.0f) * (1.0f - t) + syn_shape(s + 1, ph, 0.0f) * t;
            cv_rect(16 + i, 12 - (int32_t)(v * 9.0f), 1, 2, K_TEAL);
        }
    }
    cv_text(224 - text_w(&FONT_S, lv), (24 - (int32_t)FONT_S.h) / 2, &FONT_S, lv, K_TEXT);
    cv_blit(0, y);
}

static void ui_synth(void)
{
    static const char *const WV[4] = {"sine", "triangle", "saw", "square"};
    char a[24], b[24], c[24], d[24], o[16];
    snprintf(a, sizeof a, "%s", WV[(syn.wave * 3 + 63) / 127]);
    snprintf(b, sizeof b, "%u Hz", (unsigned)synth_cutoff_hz());
    snprintf(c, sizeof c, "%u ms", (unsigned)synth_attack_ms());
    snprintf(d, sizeof d, "%u ms", (unsigned)synth_release_ms());
    snprintf(o, sizeof o, "OCT %+d", syn.octave);
    ui_title(0, "Synth", o);
    ui_cell(1, 8, 42, "K1", "WAVE", a, 0);
    ui_cell(2, 120, 42, "K2", "FILTER", b, 0);
    ui_cell(3, 8, 88, "K3", "ATTACK", c, 0);
    ui_cell(4, 120, 88, "K4", "RELEASE", d, 0);
    ui_synth_wave(5, 136);
    ui_piano(6, 164, -1, syn_keys);
    ui_row(13, 214, 26, "SELECT level  OCT-/+", "HOME back", K_DIM, K_DIM, K_PANEL);
}

static int syn_knob(uint8_t *v, int32_t d)       /* a knob's detents, 2 steps each, 0..127; changed? */
{
    int32_t x = (int32_t)*v + 2 * d;
    if (!d)
        return 0;
    x = x < 0 ? 0 : x > 127 ? 127 : x;
    if (x == *v)
        return 0;
    *v = (uint8_t)x;
    return 1;
}

static void synth_leave(void)                     /* back to Home: notes let go, the settings kept */
{
    synth_all_off();
    syn_keys = 0;
    if (syn_changed) {
        syn_changed = 0;
        rset.syn = syn;
        rset_save();
    }
    ui_page_set(PG_HOME);
}
```

In `ui_draw`'s switch, before `default:`, add:

```c
    case PG_SYNTH: ui_synth(); break;
```

- [ ] **Step 6: `app/app.c` — input.** In `input()`, inside `if (ui_page == PG_HOME) {`, after the `B_PLAY` line add:

```c
        if (down & (1u << B_ARP)) {
            syn_keys = 0;
            ui_page_set(PG_SYNTH);
            return;
        }
```

Before the final `}` of the `else if (ui_page == PG_PASS) { ... }` chain, add the Synth page's branch (as a new
`else if` after the PG_PASS block):

```c
    } else if (ui_page == PG_SYNTH) {
        uint32_t keys = plat_keys(), on = keys & ~syn_keys, off = syn_keys & ~keys;
        int k, ch = 0;
        syn_keys = keys;
        for (k = 0; k < NKEYS; k++) {
            if (off >> k & 1u)
                synth_note_off(syn_key_note[k]);
            if (on >> k & 1u) {
                syn_key_note[k] = (int8_t)(53 + k + 12 * syn.octave);   /* F3 = MIDI 53 */
                synth_note_on(syn_key_note[k]);
            }
        }
        ch |= syn_knob(&syn.wave, plat_enc(EN_K1));
        ch |= syn_knob(&syn.cutoff, plat_enc(EN_K2));
        ch |= syn_knob(&syn.attack, plat_enc(EN_K3));
        ch |= syn_knob(&syn.release, plat_enc(EN_K4));
        ch |= syn_knob(&syn.level, turn);
        if ((down & (1u << B_OCTDN)) && syn.octave > -2)
            syn.octave--, ch = 1;
        if ((down & (1u << B_OCTUP)) && syn.octave < 2)
            syn.octave++, ch = 1;
        if (ch) {
            synth_update();
            syn_changed = 1;
        }
        if (down & ((1u << B_HOME) | (1u << B_ARP)))
            synth_leave();
```

- [ ] **Step 7: `app/app.c` — lights.** In `lights()`, before `plat_leds(lb, lk);` add:

```c
    if (ui_page == PG_SYNTH) {
        lb = 1u << B_HOME | 1u << B_ARP | 1u << B_OCTDN | 1u << B_OCTUP;
        lk = ((1u << NKEYS) - 1u) & ~syn_keys;    /* the keys lit, the held ones off */
    }
```

- [ ] **Step 8: `app/radio.c` — the mix.** In `radio_render`, replace

```c
        if (playing)
            fx_process(&l, &rr);
        o[2u * i] = (int32_t)(l * gain);
        o[2u * i + 1u] = (int32_t)(rr * gain);
```
with
```c
        if (playing)
            fx_process(&l, &rr);
        {   /* the synth over it (app/synth.c), the sum held to full scale */
            float sy = synth_next();
            l = fm_clampf(l + sy, -1.0f, 1.0f);
            rr = fm_clampf(rr + sy, -1.0f, 1.0f);
        }
        o[2u * i] = (int32_t)(l * gain);
        o[2u * i + 1u] = (int32_t)(rr * gain);
```

- [ ] **Step 9: `app/app.c` — `app_expect`** (host only). After `app_input_only` add:

```c
#ifdef PLAT_HOST
int app_expect(const char *what, int *got)       /* the host simulator's checks (tests/*.host) */
{
    int i, n = 0;
    if (!strcmp(what, "page"))
        *got = ui_page;
    else if (!strcmp(what, "syn_wave"))
        *got = syn.wave;
    else if (!strcmp(what, "syn_cutoff"))
        *got = syn.cutoff;
    else if (!strcmp(what, "syn_attack"))
        *got = syn.attack;
    else if (!strcmp(what, "syn_release"))
        *got = syn.release;
    else if (!strcmp(what, "syn_level"))
        *got = syn.level;
    else if (!strcmp(what, "syn_octave"))
        *got = syn.octave;
    else if (!strcmp(what, "syn_voices")) {
        for (i = 0; i < SYN_VOICES; i++)
            n += syv[i].on;
        *got = n;
    } else
        return 0;
    return 1;
}
#endif
```

- [ ] **Step 10: `app/radio_ui.c` — help.** Replace `ui_help` with:

```c
static void ui_help(void)
{
    static const char *const L[9][2] = {
        {"KNOB 1..4", "continent .. station"}, {"SELECT", "next station"}, {"SEL", "browse the world"},
        {"PLAY", "play / stop"}, {"SAVE", "favorites"}, {"ARP", "synth on the keys"}, {"GLO", "WiFi, networks"},
        {"MASTER", "volume"}, {"keys / FX", "hold for FX / tap tempo"}};
    int i;
    ui_title(0, "Controls", "");
    for (i = 0; i < 9; i++)
        ui_row(1 + i, 40 + 19u * (uint32_t)i, 19, L[i][0], L[i][1], K_TEXT, K_DIM, K_BG);
    ui_row(13, 214, 26, "let go of HOME", "", K_DIM, K_DIM, K_PANEL);
}
```

(SAVE's page arrives in Task 4; until then SAVE does nothing, which is harmless.)

- [ ] **Step 11: Run the tests to see them pass, and build all three**

Run: `sh tests/run_tests.sh` → every `expect` line `  ok`, `all tests passed`.
Run: `PYTHON=/opt/homebrew/bin/python3.14 ./build.sh && ./build.sh --web` → `package ... radio.fwsc`, `emu ...`.
Check the build's `ok    image ... RAM .data+.bss` line: RAM must stay under 131072.

- [ ] **Step 12: Commit**

```bash
git add app/app.c app/app.h app/radio.c app/radio_ui.c tests/synth.host
git commit -m "Synth page (ARP): the keys play app/synth.c over the radio; K1..K4 wave, filter, attack, release; SELECT
its level; OCT-/+; kept with the last station

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UGtNzPdY7XxqMyvqRESveN"
```

---

### Task 3: The favorites' list, alone, with its unit test

**Goal:** `app/fav.c` holds the favorites' type and list operations, proven by a `cc` unit test.

**Files:**
- Create: `app/fav.c`
- Create: `tests/fav_test.c`

**Acceptance Criteria:**
- [ ] 16 adds succeed; the 17th returns -1 (full)
- [ ] Adding a station already there returns -2; an empty station id returns -3
- [ ] Deleting the middle one keeps the others in order; deleting past the end does nothing
- [ ] `fav_valid` rejects a wrong magic, a wrong length, a count over 16

**Verify:** `sh tests/run_tests.sh` → `fav_test: all passed`

**Steps:**

- [ ] **Step 1: Write the failing test** `tests/fav_test.c`:

```c
/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* app/fav.c alone: cc -O1 -Ibase/include -o build/fav_test tests/fav_test.c && ./build/fav_test */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../app/fav.c"

static int bad;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "  ok" : "FAIL", what);
    bad += !ok;
}
static fav_t mk(int i)
{
    fav_t e;
    memset(&e, 0, sizeof e);
    e.country = (uint16_t)i;
    snprintf(e.place, sizeof e.place, "place %d", i);
    snprintf(e.st_id, sizeof e.st_id, "id%d", i);
    snprintf(e.title, sizeof e.title, "station %d", i);
    return e;
}

int main(void)
{
    static fav_list_t l;
    fav_t e;
    int i, ok = 1;
    fav_clear(&l);
    for (i = 0; i < 16; i++) {
        e = mk(i);
        ok &= fav_add(&l, &e) == 0;
    }
    check(ok && l.n == 16, "16 added");
    e = mk(99);
    check(fav_add(&l, &e) == -1, "the 17th: full");
    fav_clear(&l);
    e = mk(1);
    fav_add(&l, &e);
    check(fav_add(&l, &e) == -2, "a station already there");
    e.st_id[0] = 0;
    check(fav_add(&l, &e) == -3, "no station id");
    e = mk(2), fav_add(&l, &e);
    e = mk(3), fav_add(&l, &e);
    fav_del(&l, 1);
    check(l.n == 2 && !strcmp(l.f[0].st_id, "id1") && !strcmp(l.f[1].st_id, "id3"), "delete the middle");
    fav_del(&l, 5);
    check(l.n == 2, "delete past the end: nothing");
    check(fav_find(&l, "id3") == 1 && fav_find(&l, "id2") == -1, "find");
    check(fav_valid(&l, (int)sizeof l), "valid");
    l.magic = 0;
    check(!fav_valid(&l, (int)sizeof l), "wrong magic");
    l.magic = FAV_MAGIC;
    check(!fav_valid(&l, (int)sizeof l - 1), "wrong length");
    l.n = 17;
    check(!fav_valid(&l, (int)sizeof l), "count over 16");
    printf(bad ? "fav_test: %d failed\n" : "fav_test: all passed\n", bad);
    return bad != 0;
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `sh tests/run_tests.sh`
Expected: cc error `../app/fav.c: No such file or directory`.

- [ ] **Step 3: Write** `app/fav.c`:

```c
/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Favorites: up to 16 stations in their own store object (OBJ_FAV), loaded only while their page (SAVE) is open.
 * The list's operations here, apart from the store and the screen, so tests/fav_test.c can try them alone. Part of
 * the app's unity build (app.c, before radio_ui.c). */
#include <stdint.h>                              /* (memset, strcmp: the app's; tests/fav_test.c includes string.h) */

#define FAV_MAX 16u
#define FAV_MAGIC 0x46415631u                     /* "FAV1" */
typedef struct {
    uint16_t country;                             /* the places table's index */
    char place[32], st_id[12], title[32];
} fav_t;
typedef struct {
    uint32_t magic;
    uint16_t n, rsv;
    fav_t f[FAV_MAX];
} fav_list_t;

static fav_list_t *fav;                           /* the page's copy, from the heap; 0 while it is closed */
static int fav_sel;                               /* 0 the add row, 1.. the favorites */

static void fav_clear(fav_list_t *l)
{
    memset(l, 0, sizeof *l);
    l->magic = FAV_MAGIC;
}
static int fav_valid(const fav_list_t *l, int len)   /* a loaded object, whole and ours */
{
    return len == (int)sizeof *l && l->magic == FAV_MAGIC && l->n <= FAV_MAX;
}
static int fav_find(const fav_list_t *l, const char *st_id)
{
    uint32_t i;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->f[i].st_id, st_id))
            return (int)i;
    return -1;
}
static int fav_add(fav_list_t *l, const fav_t *e)  /* 0 added; -1 full, -2 there already, -3 no station */
{
    if (!e->st_id[0])
        return -3;
    if (fav_find(l, e->st_id) >= 0)
        return -2;
    if (l->n >= FAV_MAX)
        return -1;
    l->f[l->n++] = *e;
    return 0;
}
static void fav_del(fav_list_t *l, uint32_t i)
{
    if (i >= l->n)
        return;
    for (; i + 1u < l->n; i++)
        l->f[i] = l->f[i + 1u];
    l->n--;
}
```

- [ ] **Step 4: Run the test to see it pass**

Run: `sh tests/run_tests.sh` → `fav_test: all passed`, `all tests passed`.

- [ ] **Step 5: Commit**

```bash
git add app/fav.c tests/fav_test.c
git commit -m "Favorites' list (app/fav.c): up to 16, add / delete / find / valid; its unit test

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UGtNzPdY7XxqMyvqRESveN"
```

---

### Task 4: The Favorites page and its store slot

**Goal:** SAVE on Home opens the Favorites page: add the current station, tune a favorite, delete with REC; kept in OBJ_FAV at 0xD6000, the places area ending there.

**Files:**
- Modify: `app/app.h` (OBJ_FAV, the map)
- Modify: `app/places.c` (RGP_END), `app/rg.c` (RG_MAX)
- Modify: `app/app.c` (include fav.c; input; lights; ui_fav; ui_draw; app_expect)
- Modify: `app/radio_ui.c` (`fav_tune`, `fav_can_add`, `fav_entry`; the dump's line)
- Create: `tests/fav.host`

**Acceptance Criteria:**
- [ ] Host: `tap SAVE` → `expect page 7`, `fav_open 1`, `fav_n 0`; with no station (no places on the host) SEL adds nothing (`fav_n 0`); `tap HOME` → page 0, `fav_open 0`
- [ ] The places area ends at 0xD6000 (`grep -n 0xD6000 app/places.c app/rg.c app/app.h` shows all three)
- [ ] Device: the dump shows `fav N sel S` while the page is open; add, tune, delete work; a favorite is still there after a power cycle
- [ ] Device, host and web builds succeed; RAM under 131072

**Verify:** `sh tests/run_tests.sh` → `== tests/fav.host` all `  ok`, `all tests passed`

**Steps:**

- [ ] **Step 1: Write the failing host script** `tests/fav.host`:

```
# the Favorites page: SAVE opens it, HOME closes it; with no places table (the host has none) nothing to add
wait 500
expect page 0
tap SAVE
wait 50
expect page 7
expect fav_open 1
expect fav_n 0
tap SEL
wait 50
expect fav_n 0
tap HOME
wait 50
expect page 0
expect fav_open 0
```

- [ ] **Step 2: Run it to see it fail**

Run: `sh tests/run_tests.sh` → `FAIL expect page 7, got 0`.

- [ ] **Step 3: The store slot.** `app/app.h`: replace the store comment, enum and map with:

```c
/* the store: the panel calibration (the platform's), the radio's address and calibration (OBJ_BLE), the networks
 * kept (OBJ_WIFI), the station of the last session and the synth's settings (OBJ_RADIO), the favorites (OBJ_FAV:
 * the places area ends below it) */
enum { OBJ_PANEL, OBJ_BLE, OBJ_WIFI, OBJ_RADIO, OBJ_FAV, OBJ_NOBJ };
#define APP_STORE_MAP {0xDC000u, 0xDE000u, 0xDA000u, 0xD8000u, 0xD6000u}
```

`app/places.c`: replace
```c
#define RGP_END 0xD8000u                           /* .. OBJ_RADIO's sectors (app.h) */
```
with
```c
#define RGP_END 0xD6000u                           /* .. OBJ_FAV's sectors (app.h): 258 KB for a table of ~130 */
```

`app/rg.c`: replace
```c
#define RG_MAX (0xD8000u - PLAT_DATA_LO)          /* .. OBJ_RADIO's sectors */
```
with
```c
#define RG_MAX (0xD6000u - PLAT_DATA_LO)          /* .. OBJ_FAV's sectors (app.h) */
```

- [ ] **Step 4: `app/app.c` — include the list.** Immediately before `#include "radio_ui.c"` insert:

```c
#include "fav.c"
```

- [ ] **Step 5: `app/radio_ui.c` — the station's side.** After `station_tune()` add:

```c
/* ---- favorites (app/fav.c): the current station as one, and one tuned as the start restores the last */
static int fav_can_add(void)
{
    return fav && sel_valid() && st_id[0] && fav_find(fav, st_id) < 0 && fav->n < FAV_MAX;
}
static void fav_entry(fav_t *e)
{
    memset(e, 0, sizeof *e);
    e->country = sel_country;
    w_cpy(e->place, place_name, sizeof e->place);
    w_cpy(e->st_id, st_id, sizeof e->st_id);
    w_cpy(e->title, st_title, sizeof e->title);
}
static int fav_tune(const fav_t *e)               /* 0 tuned; -1 its place is gone from the table */
{
    int32_t p;
    if (!sel_valid() || e->country >= rg->n_country || (p = rg_place_index(e->country, e->place)) < 0)
        return -1;
    sel_country = e->country;
    sel_cont = rg_country(sel_country)->cont;
    sel_place = (uint16_t)p;
    place_load();
    w_cpy(st_id, e->st_id, sizeof st_id);
    w_cpy(st_title, e->title, sizeof st_title);
    tune_want = TW_NONE;
    radio_want = 1;
    station_tune();                               /* the old stream stopped; this one starts once the network is
                                                   * free (radio_ui_service), and is remembered as the last */
    return 0;
}
```

In `radio_ui_dump`, before its final `}` add:

```c
    if (fav) {
        snprintf(b, sizeof b, "fav %u sel %d", (unsigned)fav->n, fav_sel);
        wire_line(b);
    }
```

Note: `radio_ui_dump` returns early when `!sel_valid()`; that is fine (the device always has the places).

- [ ] **Step 6: `app/app.c` — open, close, draw.** After `synth_leave` (Task 2) add:

```c
/* ---- the Favorites page (SAVE) */
static void fav_open(void)
{
    if (!(fav = malloc(sizeof *fav))) {
        snprintf(ui_msg, sizeof ui_msg, "no memory for favorites");
        return;
    }
    if (!fav_valid(fav, plat_store_load(OBJ_FAV, fav, sizeof *fav)))
        fav_clear(fav);
    fav_sel = 0;
    ui_top = 0;
    ui_page_set(PG_FAV);
}
static void fav_close(void)
{
    free(fav);
    fav = 0;
    ui_page_set(PG_HOME);
}
static void fav_save(void) { plat_store_save(OBJ_FAV, fav, sizeof *fav); }

static void ui_fav(void)
{
    char a[48], p[24], c[16];
    int i;
    if (!fav)
        return;
    if (fav_sel < ui_top)
        ui_top = fav_sel;
    if (fav_sel >= ui_top + 8)
        ui_top = fav_sel - 7;
    snprintf(c, sizeof c, "%u / %u", (unsigned)fav->n, FAV_MAX);
    ui_title(0, "Favorites", c);
    for (i = 0; i < 8; i++) {
        int r = ui_top + i;
        uint16_t bg = r == fav_sel ? K_PANEL : K_BG;
        uint32_t y = 40u + 21u * (uint32_t)i;
        if (r == 0) {
            if (fav->n >= FAV_MAX)
                snprintf(a, sizeof a, "Favorites full (%u)", FAV_MAX);
            else
                snprintf(a, sizeof a, "+ Add %.20s", st_title[0] ? st_title : "(no station)");
            ui_row(1 + i, y, 21, a, "", fav_can_add() ? K_TEAL : K_DIM, K_DIM, bg);
        } else if (r <= (int)fav->n) {
            const fav_t *e = &fav->f[r - 1];
            snprintf(a, sizeof a, "%.18s", e->title);
            snprintf(p, sizeof p, "%.12s", e->place);
            ui_row(1 + i, y, 21, a, p, K_TEXT, K_DIM, bg);
        } else {
            ui_row(1 + i, y, 21, "", "", K_TEXT, K_DIM, K_BG);
        }
    }
    ui_row(13, 214, 26, fav_sel ? "SEL play  REC delete" : "SEL add", ui_msg[0] ? ui_msg : "HOME back", K_DIM,
           K_DIM, K_PANEL);
}
```

In `ui_draw`'s switch, before `default:`, add:

```c
    case PG_FAV: ui_fav(); break;
```

- [ ] **Step 7: `app/app.c` — input.** In the Home branch, next to the `B_ARP` lines, add:

```c
        if (down & (1u << B_SAVE)) {
            fav_open();
            return;
        }
```

Add the Favorites page's branch after the PG_SYNTH branch:

```c
    } else if (ui_page == PG_FAV && fav) {
        int n = (int)fav->n + 1;
        if (turn) {
            fav_sel += turn;
            fav_sel = fav_sel < 0 ? 0 : fav_sel >= n ? n - 1 : fav_sel;
        }
        if (down & (1u << B_SEL)) {
            if (fav_sel == 0) {
                if (fav_can_add()) {
                    fav_t e;
                    fav_entry(&e);
                    if (fav_add(fav, &e) == 0) {
                        fav_save();
                        fav_sel = (int)fav->n;
                    }
                }
            } else if (fav_tune(&fav->f[fav_sel - 1]) == 0) {
                fav_close();
                return;
            } else {
                snprintf(ui_msg, sizeof ui_msg, "place gone");
            }
        }
        if ((down & (1u << B_REC)) && fav_sel > 0) {
            fav_del(fav, (uint32_t)(fav_sel - 1));
            fav_save();
            if (fav_sel > (int)fav->n)
                fav_sel = (int)fav->n;
        }
        if (down & ((1u << B_HOME) | (1u << B_SAVE)))
            fav_close();
```

In `lights()`, before `plat_leds(lb, lk);` add:

```c
    if (ui_page == PG_FAV)
        lb = 1u << B_SEL | 1u << B_HOME | 1u << B_SAVE | (fav_sel > 0 ? 1u << B_REC : 0u);
```

- [ ] **Step 8: `app_expect`** — add before `} else return 0;` in `app_expect`:

```c
    } else if (!strcmp(what, "fav_open")) {
        *got = fav != 0;
    } else if (!strcmp(what, "fav_n")) {
        *got = fav ? (int)fav->n : -1;
    } else if (!strcmp(what, "fav_sel")) {
        *got = fav_sel;
```

(placed so the chain stays `if … else if … else return 0;`).

- [ ] **Step 9: Run the tests, build all three**

Run: `sh tests/run_tests.sh` → all `  ok`, `all tests passed`.
Run: `PYTHON=/opt/homebrew/bin/python3.14 ./build.sh && ./build.sh --web` → both succeed; RAM under 131072.
Run: `grep -n 0xD6000 app/places.c app/rg.c app/app.h` → three lines.

- [ ] **Step 10: Commit**

```bash
git add app/app.c app/app.h app/radio_ui.c app/places.c app/rg.c tests/fav.host
git commit -m "Favorites page (SAVE): add the current station, SEL plays one, REC deletes; kept in OBJ_FAV (0xD6000), the
places area ending there

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UGtNzPdY7XxqMyvqRESveN"
```

---

### Task 5: On the FM-1, then push

**Goal:** Both features working on the FM-1, the audio unharmed, then pushed.

**Files:**
- Modify: `README.md` (the controls: SAVE favorites, ARP synth)

**Acceptance Criteria:**
- [ ] Flashed build: an AAC or 320 kbit/s station plays; on the Synth page 4 keys held for 30 s add no underruns (`radio_ctl.py dump` "under" unchanged) and the heap is unchanged
- [ ] Favorites: SAVE, SEL adds (`dump` → `fav 1 sel 1`), HOME; another station tuned; SAVE, SELECT to it, SEL → that favorite plays (`dump` "station" line); REC on it → `fav 0`
- [ ] A favorite added, then a full power-off and on (the user's hands): still there
- [ ] README lists SAVE and ARP; pushed to origin/main

**Verify:** `python3 app/tools/radio_ctl.py dump` lines as above; `git log origin/main -1` shows the README commit

**Steps:**

- [ ] **Step 1: Flash** (from the worktree, its base clean on Radio's pin):
  `python3 base/tools/fm1_install.py app/build/radio.fwsc --yes` → `done: the FM-1 runs FM-1_1300`.
- [ ] **Step 2: Synth under load.** With a station playing: `python3 base/tools/fm1_remote.py tap ARP`, then
  `python3 base/tools/fm1_remote.py key 0 down key 4 down key 7 down key 12 down wait 30000 release-all`; before and
  after, `python3 app/tools/radio_ctl.py dump` → `under` and `heap ... used` the same.
- [ ] **Step 3: Favorites.** `fm1_remote.py tap HOME tap SAVE tap SEL`; `radio_ctl.py dump` → `fav 1 sel 1`;
  `fm1_remote.py tap HOME`; `radio_ctl.py turn 3 1` (another station); `fm1_remote.py tap SAVE turn SELECT 1 tap
  SEL`; `radio_ctl.py dump` → the first favorite's station; `fm1_remote.py tap SAVE turn SELECT 1 tap REC`; dump →
  `fav 0`.
- [ ] **Step 4: Ask the user** to add a favorite, switch fully off and on, and check it is listed.
- [ ] **Step 5: README.** In `README.md`'s controls list, add two lines in its style:
  "SAVE — favorites: + Add the station playing, SEL plays one, REC deletes it" and
  "ARP — a small synth on the keys over the radio: K1 wave, K2 filter, K3 attack, K4 release, SELECT level, OCT-/+".
- [ ] **Step 6: Commit and push**

```bash
git add README.md
git commit -m "README: SAVE's favorites, ARP's synth

Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01UGtNzPdY7XxqMyvqRESveN"
git push origin main
```
