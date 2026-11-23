/* The game's sounds, made at start-up (no sample files): menu ticks, the flub of a missed note,
 * Hero Power, the drummer's sticks counting in, the stars on the results. Mono, 44.1 kHz. */
#include <stdlib.h>

#include "hero.h"

Sound sfx[SFX_N];

static int16_t *make(Sound *s, float secs) {
    s->len = (unsigned)(secs * RATE);
    s->data = calloc(s->len, sizeof *s->data);
    if (!s->data) s->len = 0;
    return s->data;
}

static float env(float t, float attack, float decay) { return t < attack ? t / attack : fexp(-(t - attack) / decay); }

static void put(Sound *s, unsigned i, float v) {
    if (i >= s->len) return;
    int x = s->data[i] + (int)(v * 32767);
    s->data[i] = (int16_t)clampi(x, -32767, 32767);
}

/* a tone of f Hz with harmonics falling as 1/k^fall */
static void tone(Sound *s, float start, float f, float secs, float amp, float attack, float decay, int harmonics, float fall) {
    unsigned a = (unsigned)(start * RATE), n = (unsigned)(secs * RATE);
    for (unsigned i = 0; i < n; ++i) {
        float t = (float)i / RATE, v = 0, e = env(t, attack, decay);
        for (int k = 1; k <= harmonics; ++k) {
            float g = 1;
            for (int q = 1; q < k; ++q) g /= fall;
            v += g * fsin(TAU * f * k * t);
        }
        put(s, a + i, v * e * amp);
    }
}

static void noise(Sound *s, float start, float secs, float amp, float attack, float decay, float bright) {
    unsigned a = (unsigned)(start * RATE), n = (unsigned)(secs * RATE);
    float lp = 0, hp = 0;
    for (unsigned i = 0; i < n; ++i) {
        float t = (float)i / RATE, w = rndf() * 2 - 1;
        lp += (w - lp) * bright;
        float h = lp - hp;
        hp += (lp - hp) * 0.05f;
        put(s, a + i, h * env(t, attack, decay) * amp);
    }
}

void sfx_make(void) {
    Sound *s;
    if (make(s = &sfx[SFX_TICK], 0.03f)) tone(s, 0, 1800, 0.03f, 0.18f, 0.001f, 0.006f, 2, 3);
    if (make(s = &sfx[SFX_SELECT], 0.16f)) tone(s, 0, 880, 0.08f, 0.2f, 0.002f, 0.03f, 3, 2), tone(s, 0.06f, 1320, 0.1f, 0.2f, 0.002f, 0.04f, 3, 2);
    if (make(s = &sfx[SFX_BACK], 0.14f)) tone(s, 0, 660, 0.07f, 0.18f, 0.002f, 0.03f, 3, 2), tone(s, 0.05f, 440, 0.09f, 0.18f, 0.002f, 0.04f, 3, 2);
    /* the flub: a muted, buzzing, detuned pair of low strings */
    if (make(s = &sfx[SFX_MISS], 0.22f)) {
        float lp = 0;
        for (unsigned i = 0; i < s->len; ++i) {
            float t = (float)i / RATE, p1 = t * 98, p2 = t * 103.5f;
            float saw = (p1 - (int)p1) * 2 - 1 + (p2 - (int)p2) * 2 - 1;
            float buzz = saw + (rndf() - 0.5f) * 0.6f;
            lp += (buzz - lp) * 0.08f;
            put(s, i, lp * env(t, 0.003f, 0.06f) * 0.45f);
        }
    }
    if (make(s = &sfx[SFX_OVER], 0.12f)) {
        float lp = 0;
        for (unsigned i = 0; i < s->len; ++i) {
            float t = (float)i / RATE, p = t * 140;
            lp += (((p - (int)p) * 2 - 1) - lp) * 0.1f;
            put(s, i, lp * env(t, 0.002f, 0.03f) * 0.3f);
        }
    }
    if (make(s = &sfx[SFX_HERO_READY], 0.5f)) {
        static const float f[] = { 1046.5f, 1318.5f, 1568.0f, 2093.0f };
        for (int k = 0; k < 4; ++k) tone(s, k * 0.06f, f[k], 0.3f, 0.12f, 0.002f, 0.12f, 2, 4);
    }
    if (make(s = &sfx[SFX_HERO_GO], 0.9f)) {
        unsigned n = s->len;
        float lp = 0;
        for (unsigned i = 0; i < n; ++i) { /* a rising whoosh */
            float t = (float)i / RATE, b = 0.02f + 0.25f * t;
            lp += ((rndf() * 2 - 1) - lp) * b;
            put(s, i, lp * env(t, 0.25f, 0.25f) * 0.5f);
        }
        static const float f[] = { 261.6f, 329.6f, 392.0f, 523.3f };
        for (int k = 0; k < 4; ++k) tone(s, 0.18f, f[k], 0.7f, 0.08f, 0.01f, 0.3f, 4, 1.6f);
    }
    if (make(s = &sfx[SFX_STICK], 0.06f)) {
        tone(s, 0, 2400, 0.04f, 0.25f, 0.0005f, 0.008f, 1, 1);
        noise(s, 0, 0.05f, 0.5f, 0.0005f, 0.01f, 0.6f);
    }
    if (make(s = &sfx[SFX_STAR], 0.4f)) tone(s, 0, 1568, 0.4f, 0.16f, 0.002f, 0.12f, 3, 3), tone(s, 0, 2349, 0.4f, 0.08f, 0.002f, 0.1f, 2, 3);
    if (make(s = &sfx[SFX_FAIL], 1.1f)) {
        static const float f[] = { 392, 370, 349.2f, 311.1f };
        for (int k = 0; k < 4; ++k) tone(s, k * 0.22f, f[k], k == 3 ? 0.4f : 0.22f, 0.16f, 0.005f, k == 3 ? 0.25f : 0.1f, 5, 1.5f);
    }
    if (make(s = &sfx[SFX_CLICK], 0.05f)) tone(s, 0, 1500, 0.05f, 0.35f, 0.0005f, 0.01f, 1, 1), noise(s, 0, 0.02f, 0.3f, 0.0005f, 0.004f, 0.9f);
}
