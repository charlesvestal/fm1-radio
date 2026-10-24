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
