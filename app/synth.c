/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* A small synth for the keys (ARP's page): 4 voices, each one oscillator whose wave morphs sine -> triangle -> saw
 * -> square (a crossfade between neighbours; saw and square band-limited, polyBLEP) and an attack / release
 * envelope (full sustain while held); one 2-pole low-pass (state-variable, trapezoidal: stable at any cutoff) on
 * their mix, then a soft limit (tanh). synth_next() runs in the audio interrupt (radio_render), a sample at a time;
 * the rest from the main loop. Settings 0..127. Part of the app's unity build; tests/synth_test.c builds it alone. */
#include <stdint.h>
#include "fastmath.h"

#define SYN_VOICES 4
#define SYN_SR 44100.0f
typedef struct { uint8_t wave, cutoff, attack, release, level; int8_t octave; } synth_set_t;
static const synth_set_t SYN_DEFAULT = {42, 96, 4, 48, 110, 0};
static synth_set_t syn = {42, 96, 4, 48, 110, 0};

typedef struct {
    float ph, inc, env;                           /* phase and its step, in turns; the envelope 0..1 */
    uint8_t note, gate, on;                       /* gate: held; on: sounding (until the release ends) */
    uint32_t age;                                 /* when it was started: the oldest is stolen */
} syn_voice_t;
static syn_voice_t syv[SYN_VOICES];
static uint32_t syn_age;
static float syn_g, syn_a1, syn_a2, syn_a3, syn_ic1, syn_ic2;   /* the filter: coefficients, state */
static float syn_att, syn_rel, syn_gain;          /* envelope steps a sample; the mix's gain */
static float syn_morph;                           /* the wave's morph 0..3, slewed toward the knob (no zipper) */

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
    syn_gain = 0.5f * (float)syn.level / 127.0f;  /* a voice at full: -6 dBFS (at -12 it vanished under a station);
                                                   * chords saturate softly (synth_next) */
}

static void synth_init(void)
{
    uint32_t i;
    for (i = 0; i < SYN_VOICES; i++)
        syv[i].on = syv[i].gate = 0, syv[i].env = 0.0f, syv[i].ph = 0.0f;
    syn_ic1 = syn_ic2 = 0.0f;
    syn_morph = (float)syn.wave * (3.0f / 127.0f);
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
    float m, t, sum = 0.0f, v1, v2, v3;
    int s, any = 0;
    syn_morph += ((float)syn.wave * (3.0f / 127.0f) - syn_morph) * 0.001f;   /* ~23 ms to settle */
    m = syn_morph;
    s = (int)m;
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
    return fm_tanhf(v2);                          /* a soft limit: 4 voices at full stay under full scale */
}
