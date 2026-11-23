/* The notes for one difficulty, laid out the way hand-made rhythm game charts are.
 *
 * Eleven osu!mania maps that play well (studied in test/maps.py, with their songs) are built on
 * the beat, not on onsets: 92% of their beats carry a note and 60-65% of those are chords, 77% of
 * half-beats carry one (15% chords), quarter-beats 11-17% (as fills where the music is busy). About
 * 70% of bars reuse a rhythm heard earlier in the song, and their lanes move by shape (steps,
 * zigzags, repeats), not by the tune's pitch, which they follow no more often than not.
 *
 *   1. The grid: every quarter of every beat analyze.c found.
 *   2. Each slot is scored by a model fitted to those maps: its place (beat, half, quarter) and
 *      what sounds there (the held and struck rises, the loudness), each to the song's own level.
 *      One model for whether a note is there, one for whether it is a chord. Cross-validated over
 *      the eleven songs, notes chosen this way sit on a mapper's note 87% of the time.
 *   3. Each difficulty keeps the best share of beats, half-beats and quarter-beats it plays, no
 *      two nearer than its least gap; chords go on its best-scored beats.
 *   4. A bar that sounds like an earlier one (what its sound adds to the place's score) takes that
 *      bar's rhythm and chords, so the song's sections play alike, as in the maps.
 *   5. Lanes come from shapes (a bounce, a zigzag, an anchor on Centre, stairs), one a bar; a bar
 *      that repeats an earlier one repeats its shape, mirrored, as mappers vary a repeat.
 *   6. A note on a held sound of the tune or bass, with room after it, is held.
 *
 * Q2 Pod's own rules shape the lanes (README.md): a Centre press that begins while Prev
 * or Next is held is dropped as a slip of the thumb, so the only chord is Prev + Next, nothing
 * comes inside a held note, and a Centre note comes a little after a side one; and Centre held 5 s
 * opens the Shut Down prompt, so a held Centre note ends long before. */
#include <stdlib.h>
#include <string.h>

#include "hero.h"

const char *const diff_name[DIFFS] = { "Easy", "Medium", "Hard", "Expert" };

static const struct {
    float q[3];        /* the share of beats, half-beats and quarter-beats kept, best first */
    float gap;         /* least s between notes */
    float cap;         /* most notes a second, over the song */
    float chord;       /* the share of the kept beats that are chords */
    float dtap;        /* least s between two presses of one lane */
    float side_centre; /* least s from a side note to a Centre one */
    float window;      /* s either side */
    float miss_drain;
    float held;        /* the share of notes held, at most */
    float peak;        /* most notes in any second */
    float by_partial;  /* how a held note found by its partials (strings) ranks against a sung one */
} P[DIFFS] = { /* a Q2 button is pressed again comfortably 4 times a second, not the 10 of a keyboard.
                  Held notes measured on mixes with a known tune and strings (test/tunes.py): on Hard
                  and Expert, held notes found by their partials alone were as often wrong as right */
    { { 0.75f, 0.00f, 0.00f }, 0.42f, 1.3f, 0.00f, 0.40f, 0.40f, 0.115f, 0.045f, 0.60f, 2.5f, 0.8f },
    { { 0.88f, 0.30f, 0.00f }, 0.24f, 2.3f, 0.10f, 0.30f, 0.25f, 0.105f, 0.055f, 0.48f, 3.5f, 0.8f },
    { { 0.92f, 0.60f, 0.04f }, 0.15f, 3.6f, 0.28f, 0.25f, 0.17f, 0.095f, 0.065f, 0.30f, 5.0f, 0.2f },
    { { 0.94f, 0.77f, 0.14f }, 0.10f, 5.2f, 0.45f, 0.22f, 0.15f, 0.085f, 0.075f, 0.25f, 6.0f, 0.2f },
};

/* fitted to the maps (test/maps.py): beat, half, quarter; the bass, middle and high held rises,
 * kick, snare, hats, loudness; half x kick, snare, hats; quarter x middle, kick, snare, hats; bias */
/* a quarter-beat fill needs this much of a note's score: none where nothing sounds (test/maps.py:
 * the maps' notes among Expert's went from 85% to 86%) */
#define FILL_FLOOR 0.25f
#define HALF_FLOOR 0.0f
#define NF 18
#define LEAD_W 1.0f /* the lead's weight in a slot's score (against the maps' model, 0-1) */
/* where a bar has a lead part (a voice, else the tune, else the bass), each difficulty plays its
 * notes up to a place in the beat (Easy the beat, Medium the half, Hard and Expert the quarter),
 * and of the other places, a share for the pulse */
#define LEAD_MIN 0.25f
static const struct {
    int cls;
    float pulse[3];
} LEAD[DIFFS] = {
    { 0, { 0.35f, 0.00f, 0.00f } }, /* measured on mixes with a known voice (test/tunes.py): more */
    { 1, { 0.42f, 0.07f, 0.00f } }, /* pulse charted no more of the voice, and buried it */
    { 2, { 0.49f, 0.21f, 0.00f } },
    { 2, { 0.56f, 0.35f, 0.04f } },
};
static const float W_ROW[NF] = { 0.8046f, -0.1419f, -1.2533f, 0.3082f, -0.0072f, -0.1026f, 0.4843f, 1.1234f, 1.0850f,
                                 0.4529f, 0.1499f, 0.2072f, 0.2587f, -0.7137f, -0.4964f, -0.1039f, -0.1916f, -1.5638f };
static const float W_CHORD[NF] = { 0.8885f, -0.6182f, -1.0405f, 0.3699f, -0.1012f, -0.0649f, 0.2785f, 0.7351f, 0.3723f,
                                   0.3464f, -0.0726f, -0.0827f, -0.1078f, -0.6911f, -0.6293f, -0.5831f, -0.5957f, -2.2328f };

/* lane shapes, one a bar: Prev 0, Centre 1, Next 2; -1 ends one */
static const int8_t SHAPE[][6] = {
    { 0, 1, 2, 1, -1 }, /* a bounce */
    { 0, 2, -1 },       /* a zigzag */
    { 1, 0, 1, 2, -1 }, /* an anchor on Centre */
    { 0, 1, 2, -1 },    /* stairs */
    { 2, 1, 0, 1, -1 }, /* a bounce down */
    { 1, 2, 1, 0, -1 }, /* the anchor the other way */
};
#define SHAPES (int)(sizeof SHAPE / sizeof SHAPE[0])

typedef struct {
    float t, p, pc, res, sel; /* sel: p, partly against the slot's neighbours, for choosing */
    int cls;  /* 0 beat, 1 half, 2 quarter */
    int beat; /* the beat it is in */
    int keep, chord;
    int lead_bar;           /* the bar has a lead part */
    int by_lead;            /* kept as a note of the lead: not to be copied over, nor held over */
    float wob, bass, lead;  /* the tune note's wobble; a bass note begun here, its strength; how much
                               the bar's lead part (voice, else tune, else bass) begins a note here */
    float tune, pitch, sus; /* a tune note begun here: its strength (0: none) and pitch; how long the
                               tune (or else the bass) note begun here sounds, s */
    float hold;             /* the note is held this long, s */
    int under;              /* inside a held side note: played on the other side */
    float by_level;         /* the length is a held sound's (strings, a pad, a voice the tune missed): its rank weight */
    int strong;             /* held long enough to take the notes under it (Easy, Medium, a chord) */
    int needs_side;         /* held with notes inside: on Prev or Next */
} Slot;

static int cmp_f(const void *a, const void *b) {
    float d = *(const float *)a - *(const float *)b;
    return d < 0 ? -1 : d > 0;
}
static float median_of(float *v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp_f);
    return n ? v[n / 2] : 0;
}
static float sigmoid(float x) { return 1 / (1 + fexp(-x)); }

/* the slot's sound: the most of each rise within 35 ms, the loudness on average */
static void slot_sound(const Analysis *a, float t, float *f) {
    float t0 = hop_seconds(0), per = hop_seconds(1) - t0;
    int c = (int)((t - t0) / per + 0.5f), lo = c - 3, hi = c + 3, k = 0;
    for (int j = 0; j < 7; ++j) f[j] = 0;
    for (int h = lo; h <= hi; ++h) {
        if (h < 0 || h >= a->ncurve) continue;
        const uint8_t *v = a->curve + h * CURVES;
        for (int j = 0; j < 6; ++j) f[j] = maxf(f[j], v[j] / 40.0f);
        f[6] += v[6] / 150.0f, ++k;
    }
    if (k) f[6] /= k;
}

/* How long a sound begun at t holds, s: the longest partial begun within 35 ms before to 70 ms
 * after (analyze.c: a peak that stays within 6 dB at its pitch), a voice's or strings' long note
 * that the tune's notes can miss */
static float held_for(const Analysis *a, float t) {
    float t0 = hop_seconds(0), per = hop_seconds(1) - t0;
    int h0 = (int)((t - t0) / per + 0.5f), best = 0;
    for (int h = h0 - 3; h <= h0 + 6; ++h)
        if (h >= 0 && h < a->ncurve && a->curve[h * CURVES + 7] > best) best = a->curve[h * CURVES + 7];
    return best * 4 * per;
}

/* indexes of slots of class c, best p first */
static Slot *sort_base;
static int by_p_desc(const void *x, const void *y) {
    float d = sort_base[*(const int *)y].sel - sort_base[*(const int *)x].sel;
    return d < 0 ? -1 : d > 0;
}
static int by_pc_desc(const void *x, const void *y) {
    float d = sort_base[*(const int *)y].pc - sort_base[*(const int *)x].pc;
    return d < 0 ? -1 : d > 0;
}

/* too many for the difficulty (a fast song): the weakest go, quarter-beats first, then halves */
static void cap_density(Slot *s, int ns, int *idx, int most) {
    int m = 0;
    for (int i = 0; i < ns; ++i)
        if (s[i].keep) idx[m++] = i;
    for (int cl = 2; cl >= 0 && m > most; --cl) {
        int k = 0;
        for (int i = 0; i < m; ++i)
            if (s[idx[i]].cls == cl) idx[k++] = idx[i];
        sort_base = s;
        qsort(idx, (size_t)k, sizeof *idx, by_p_desc);
        for (int i = k - 1; i >= 0 && m > most; --i) s[idx[i]].keep = s[idx[i]].chord = 0, --m;
        k = 0;
        for (int i = 0; i < ns; ++i)
            if (s[i].keep) idx[k++] = i;
    }
}

/* how well a shape's moves over the bar's single notes follow the tune's: +1 for a move the same
 * way (or a repeat in place), -1 for one the other way */
static int shape_fit(const Slot *s, int from, int to, int sh, int mi) {
    int len = 0, k = 0, v = 0, last_lane = -1;
    float last_p = -1;
    while (SHAPE[sh][len] >= 0) ++len;
    for (int i = from; i < to; ++i) {
        if (!s[i].keep || s[i].chord || s[i].under) continue;
        int lane = SHAPE[sh][k++ % len];
        if (mi) lane = 2 - lane;
        if (s[i].tune > 0 && last_p >= 0) {
            float dp = s[i].pitch - last_p;
            int dl = lane - last_lane;
            if ((dp > 0.5f && dl > 0) || (dp < -0.5f && dl < 0) || (fabs_(dp) <= 0.5f && dl == 0)) ++v;
            else if ((dp > 0.5f && dl < 0) || (dp < -0.5f && dl > 0)) --v;
        }
        last_p = s[i].tune > 0 ? s[i].pitch : -1, last_lane = lane;
    }
    return v;
}

/* hold the note at slot i for len s, in whole half-beats, as far as what comes after allows:
 * the easy levels take the notes under a strong one and stop before the rest; a held chord stops
 * before the next note;
 * Hard and Expert keep single notes under it, to be played on the other side; another held
 * note, or a chord under a weak one, stops it. 1 if it is held. */
static int place_hold(Slot *s, int ns, int i, float len, float half, int diff, int strong, int keep_notes) {
    len = (float)(int)(len / half + 0.001f) * half; /* whole half-beats */
    /* a note where it would end: it ends a half-beat sooner, the note stays */
    for (int j = i + 1; j < ns && s[j].t < s[i].t + len + 0.12f; ++j)
        if (s[j].keep && s[j].t > s[i].t + len - 0.12f) {
            len -= half;
            break;
        }
    /* Hard and Expert: a sung note held over notes is held on one side, the chords under it tapped */
    if (diff >= 2 && s[i].chord && s[i].tune > 0)
        for (int j = i + 1; j < ns && s[j].t < s[i].t + len + 0.12f; ++j)
            if (s[j].keep) {
                s[i].chord = 0;
                break;
            }
    int stop = -1;
    for (int j = i + 1; j < ns && s[j].t < s[i].t + len + 0.12f; ++j) {
        if (!s[j].keep) continue;
        if (s[j].hold > 0) { stop = j; break; }
        if (s[j].by_lead && !s[i].by_lead) { stop = j; break; } /* the lead goes on over something else's */
        if (diff <= 1 && strong && !keep_notes) continue; /* taken by the held note */
        if (diff <= 1 || s[i].chord || keep_notes) { stop = j; break; }
    }
    if (diff >= 2 && stop < 0) { /* Hard, Expert: a held note gives way before it would cost a second tap */
        int lost = 0;
        float last = s[i].t;
        for (int j = i + 1; j < ns && s[j].t < s[i].t + len + 0.12f; ++j) {
            if (!s[j].keep) continue;
            if (s[j].t - last < maxf(P[diff].dtap, 0.3f) && ++lost > 1) { stop = j; break; }
            if (s[j].t - last >= maxf(P[diff].dtap, 0.3f)) last = s[j].t;
        }
    }
    if (stop >= 0) len = (float)(int)((s[stop].t - s[i].t - 0.15f) / half + 0.001f) * half;
    if (len < maxf(0.3f, half)) return 0;
    /* under a held note, a tap on the other side now and then, not a stream on one button */
    int under = 0;
    float last = s[i].t;
    for (int j = i + 1; j < ns && s[j].t < s[i].t + len + 0.12f; ++j) {
        if (!s[j].keep) continue;
        if (diff <= 1 || s[i].chord || s[j].t - last < maxf(P[diff].dtap, 0.3f)) s[j].keep = 0;
        else s[j].under = 1, s[j].chord = 0, ++under, last = s[j].t;
    }
    s[i].hold = len, s[i].needs_side = under > 0, s[i].strong = strong;
    return 1;
}

int chart_make(const Analysis *a, int diff, Chart *c) {
    memset(c, 0, sizeof *c);
    if (a->nbeats < 4 || !a->ncurve) return 0;
    diff = clampi(diff, 0, DIFFS - 1);
    const int sb = a->swing ? 3 : 4, bar = 4 * sb; /* slots a beat, a bar */
    int ns = (a->nbeats - 1) * sb;
    Slot *s = calloc((size_t)ns, sizeof *s);
    float (*f)[7] = malloc((size_t)ns * sizeof *f);
    float *tmp = malloc((size_t)ns * sizeof *tmp);
    int *idx = malloc((size_t)ns * sizeof *idx);
    if (!s || !f || !tmp || !idx) {
        free(s), free(f), free(tmp), free(idx);
        return 0;
    }
    /* 1, 2. the slots and their scores */
    for (int k = 0; k + 1 < a->nbeats; ++k)
        for (int j = 0; j < sb; ++j) {
            Slot *x = &s[k * sb + j];
            x->t = a->beat[k] + (a->beat[k + 1] - a->beat[k]) * j / sb;
            /* on thirds the swung off-beat (2/3) plays the half-beat's part, 1/3 the quarter's */
            x->cls = j == 0 ? 0 : j == sb / 2 + (sb & 1) ? 1 : 2, x->beat = k;
            slot_sound(a, x->t, f[k * sb + j]);
        }
    float med[7];
    for (int j = 0; j < 7; ++j) {
        for (int i = 0; i < ns; ++i) tmp[i] = f[i][j];
        med[j] = median_of(tmp, ns);
    }
    for (int i = 0; i < ns; ++i) {
        float A[7], x[NF];
        for (int j = 0; j < 7; ++j) A[j] = flog(1 + clampf(f[i][j] / (med[j] + 1e-3f), 0, 10));
        int c0 = s[i].cls == 0, c1 = s[i].cls == 1, c2 = s[i].cls == 2;
        x[0] = (float)c0, x[1] = (float)c1, x[2] = (float)c2;
        for (int j = 0; j < 7; ++j) x[3 + j] = A[j];
        x[10] = c1 * A[3], x[11] = c1 * A[4], x[12] = c1 * A[5];
        x[13] = c2 * A[1], x[14] = c2 * A[3], x[15] = c2 * A[4], x[16] = c2 * A[5];
        x[17] = 1;
        float zr = 0, zc = 0;
        for (int j = 0; j < NF; ++j) zr += W_ROW[j] * x[j], zc += W_CHORD[j] * x[j];
        s[i].p = sigmoid(zr), s[i].pc = sigmoid(zc);
    }
    { /* before the song begins and after it ends (the beats run on into a fade), nothing */
        int first = 0, last = ns - 1;
        while (first < ns && f[first][6] < 0.3f * med[6]) ++first;
        while (last >= 0 && f[last][6] < 0.3f * med[6]) --last;
        for (int i = 0; i < ns; ++i)
            if (i < first || i > last) s[i].p = s[i].pc = 0;
    }
    float beat_len = (a->beat[a->nbeats - 1] - a->beat[0]) / (a->nbeats - 1);
    int led_v = 0, led_t = 0, led_b = 0, led_n = 0;
    /* 2a. the tune: a note of it begun at the slot (within 45 ms), its pitch and how long it lasts;
     * the bass's length where the tune has none; or a held sound's, if longer */
    {
        int k = 0;
        for (int i = 0; i < ns; ++i) {
            while (k < a->nonsets && a->on[k].t < s[i].t - 0.045f) ++k;
            float bass = 0;
            for (int q = k; q < a->nonsets && a->on[q].t <= s[i].t + 0.045f; ++q) {
                const Onset *o = &a->on[q];
                if (o->kind == KIND_TUNE && o->strength > s[i].tune)
                    s[i].tune = o->strength, s[i].pitch = o->pitch, s[i].sus = o->dur, s[i].wob = o->wobble;
                if (o->kind == KIND_BASS) bass = maxf(bass, o->dur), s[i].bass = maxf(s[i].bass, o->strength);
            }
            if (s[i].tune <= 0) s[i].sus = 0.8f * bass;
            /* a sound held 1.5 beats or more where the tune holds nothing: strings, a pad, a voice
             * the tune missed (on mixes with a known tune and strings, string notes read a median
             * 2.0 beats, their truth 2.0; other beats 1.0) */
            float hf = held_for(a, s[i].t);
            if (hf >= 1.5f * beat_len && !(s[i].tune > 0 && s[i].sus >= 0.75f * beat_len)) {
                s[i].sus = minf(hf, 4 * beat_len), s[i].by_level = P[diff].by_partial;
            }
        }
    }
    /* 2a'. Each bar's lead: the voice where one sings (two of the bar's tune notes waver, as a voice
     * does: on mixes with a known tune, sung notes wavered a median 0.09 semitones, plucked and
     * strummed ones and strings 0.02-0.03), else the tune's instrument, else the bass. Its notes
     * lead the choice of notes; the drums keep the pulse and fill in. */
    {
        float mt = 0, mb = 0;
        int nt = 0, nbs = 0;
        for (int i = 0; i < ns; ++i) {
            if (s[i].tune > 0) tmp[nt++] = s[i].tune;
        }
        mt = nt ? median_of(tmp, nt) : 1;
        for (int i = 0; i < ns; ++i)
            if (s[i].bass > 0) tmp[nbs++] = s[i].bass;
        mb = nbs ? median_of(tmp, nbs) : 1;
        int led[4] = { 0, 0, 0, 0 }; /* bars led by the voice, the tune, the bass, none */
        for (int b0 = 0; b0 < ns; b0 += bar) {
            int voice = 0, tunes = 0, basses = 0;
            for (int i = b0; i < b0 + bar && i < ns; ++i) {
                voice += s[i].tune > 0 && s[i].wob >= 0.05f;
                tunes += s[i].tune >= 0.5f * mt;
                basses += s[i].bass >= 0.5f * mb;
            }
            for (int i = b0; i < b0 + bar && i < ns; ++i) {
                s[i].lead_bar = voice >= 2 || tunes >= 2 || basses >= 2;
                if (voice >= 2 || tunes >= 2)
                    s[i].lead = minf(s[i].tune / mt, 1.5f) / 1.5f * (voice >= 2 ? (s[i].wob >= 0.05f ? 1 : 0.6f) : 0.8f);
                else if (basses >= 2)
                    s[i].lead = minf(s[i].bass / mb, 1.5f) / 1.5f * 0.7f;
            }
            ++led[voice >= 2 ? 0 : tunes >= 2 ? 1 : basses >= 2 ? 2 : 3];
        }
        led_v = led[0], led_t = led[1], led_b = led[2], led_n = led[3];
    }
    if (diff == 0)
        logf_("chart: bars led by the voice %d, by the tune %d, by the bass %d, by the drums %d", led_v, led_t, led_b, led_n);
    /* 2b. mappers keep a section's pulse even when it is quieter than the chorus: each slot is
     * judged partly against its own class's slots nearby (8 beats either side). With half of that,
     * stretches under a third of the song's density fell from 4% to 2% on Medium, as in the maps */
    {
        const float alpha = 0.5f; /* measured (test/maps.py): fewer bare stretches, as in the maps */
        const int win = 32;
        float gm[3] = { 0, 0, 0 };
        int gn[3] = { 0, 0, 0 };
        for (int i = 0; i < ns; ++i) gm[s[i].cls] += s[i].p, ++gn[s[i].cls];
        for (int c3 = 0; c3 < 3; ++c3) gm[c3] /= gn[c3] ? gn[c3] : 1;
        for (int i = 0; i < ns; ++i) {
            float sum = 0;
            int k = 0;
            for (int j = i - win; j <= i + win; j += 1)
                if (j >= 0 && j < ns && s[j].cls == s[i].cls) sum += s[j].p, ++k;
            tmp[i] = s[i].p - alpha * (sum / k - gm[s[i].cls]);
        }
        /* and where the tune sings, a little more: the maps follow the drums, but a player hears the
         * tune. More than this changed nothing on the mixes with a known tune, and this costs the
         * maps' rhythm a point at most (test/maps.py) */
        for (int i = 0; i < ns; ++i) s[i].sel = tmp[i] + LEAD_W * s[i].lead;
    }
    /* 3. each class's best share, inside the song, with the least gap */
    int parity = -1; /* a beat faster than the gap: every other beat, the stronger set */
    if (beat_len < P[diff].gap) {
        float sum[2] = { 0, 0 };
        for (int i = 0; i < ns; i += sb) sum[s[i].beat & 1] += s[i].p;
        parity = sum[1] > sum[0];
    }
    sort_base = s;
    for (int cl = 0; cl < 3; ++cl) {
        float step = beat_len * (cl == 0 ? 1 : sb == 3 ? 1.0f / 3 : cl == 1 ? 0.5f : 0.25f);
        if (cl && step < P[diff].gap * 0.98f) continue; /* this grid is too fine for the difficulty */
        /* in a bar with a lead: every note of the lead the difficulty plays at this place, and of the
         * rest a share for the pulse; in a bar without one, the class's best share as the maps do */
        for (int led = 0; led < 2; ++led) {
            int m = 0;
            for (int i = 0; i < ns; ++i)
                if (s[i].cls == cl && s[i].p > 0 && s[i].t > 0.15f && s[i].t < a->duration - 0.8f &&
                    (cl || parity < 0 || (s[i].beat & 1) == parity) && s[i].lead_bar == led) {
                    if (led && s[i].lead >= LEAD_MIN && cl <= LEAD[diff].cls) s[i].keep = s[i].by_lead = 1;
                    else idx[m++] = i;
                }
            qsort(idx, (size_t)m, sizeof *idx, by_p_desc);
            int k = (int)((led ? LEAD[diff].pulse[cl] : P[diff].q[cl]) * m + 0.5f);
            if (cl) /* off the beat, a note only where there is something to play */
                while (k > 0 && s[idx[k - 1]].p < (cl == 2 ? FILL_FLOOR : HALF_FLOOR)) --k;
            for (int i = 0; i < k; ++i) s[idx[i]].keep = 1;
        }
    }
    { /* the least gap: the weaker of two too near goes */
        int m = 0;
        for (int i = 0; i < ns; ++i)
            if (s[i].keep) idx[m++] = i, s[i].keep = 0;
        qsort(idx, (size_t)m, sizeof *idx, by_p_desc);
        for (int q = 0; q < m; ++q) {
            int i = idx[q], clash = 0;
            for (int j = i - 1; j >= 0 && s[i].t - s[j].t < P[diff].gap && !clash; --j) clash = s[j].keep;
            for (int j = i + 1; j < ns && s[j].t - s[i].t < P[diff].gap && !clash; ++j) clash = s[j].keep;
            if (!clash) s[i].keep = 1;
        }
    }
    cap_density(s, ns, idx, (int)(P[diff].cap * a->duration));
    if (P[diff].chord > 0) { /* chords: the kept beats with the best chord scores */
        int m = 0;
        for (int i = 0; i < ns; ++i)
            if (s[i].keep && s[i].cls == 0) idx[m++] = i;
        qsort(idx, (size_t)m, sizeof *idx, by_pc_desc);
        int k = (int)(P[diff].chord * m + 0.5f);
        for (int i = 0; i < k; ++i) s[idx[i]].chord = 1;
    }
    /* 4. alike bars, alike choices: each bar's sound beyond its places' usual scores, and where its
     * tune begins notes, against the bars before it; a bar nearer an earlier one than the song's
     * median nearness takes its rhythm */
    int nbar = ns / bar;
    int *from = malloc((size_t)(nbar ? nbar : 1) * sizeof *from);
    float *near = malloc((size_t)(nbar ? nbar : 1) * sizeof *near);
    if (from && near) {
        float cm[3] = { 0, 0, 0 };
        int cn[3] = { 0, 0, 0 };
        for (int i = 0; i < ns; ++i) cm[s[i].cls] += s[i].p, ++cn[s[i].cls];
        for (int i = 0; i < ns; ++i) s[i].res = s[i].p - cm[s[i].cls] / (cn[s[i].cls] ? cn[s[i].cls] : 1);
        for (int b = 0; b < nbar; ++b) {
            from[b] = -1, near[b] = 1e9f;
            for (int e = 0; e < b; ++e) {
                float d = 0;
                for (int j = 0; j < bar; ++j) /* the sound beyond the place's usual, and the tune */
                    d += fabs_(s[b * bar + j].res - s[e * bar + j].res) +
                         0.25f * ((s[b * bar + j].tune > 0) != (s[e * bar + j].tune > 0));
                if (d < near[b]) near[b] = d, from[b] = e;
            }
        }
        int m = 0;
        for (int b = 1; b < nbar; ++b) tmp[m++] = near[b];
        float thr = m ? median_of(tmp, m) : 0;
        for (int b = 1; b < nbar; ++b) {
            if (near[b] > thr || from[b] < 0) {
                from[b] = -1;
                continue;
            }
            for (int j = 0; j < bar; ++j) {
                Slot *x = &s[b * bar + j], *y = &s[from[b] * bar + j];
                int inside = x->t > 0.15f && x->t < a->duration - 0.8f;
                if (x->by_lead && x->keep) continue; /* the lead's own notes stay */
                x->keep = y->keep && inside, x->chord = y->chord && inside;
            }
        }
        if (nbar) from[0] = -1;
    }
    cap_density(s, ns, idx, (int)(P[diff].cap * a->duration));
    /* 5. Held notes, before the lanes. A held tune note (or bass, where the tune rests) of 3/4 of a
     * beat or more, scored by how long it sounds and how quiet the next beat is (what the maps'
     * held notes follow); the best, up to the difficulty's share. Its length: what sounds, in
     * half-beats. Easy and Medium let a strong one take the notes under it; Hard and Expert keep
     * single notes under a held side note, on the other side (Q2 Pod drops a Centre press while
     * Prev or Next is down), and stop it before a chord. A held Centre note has nothing inside and
     * lasts 1.5 s at most (Centre held 5 s opens the Shut Down prompt). */
    int holds = 0;
    if (diff <= 1) { /* the easy levels sing along: a clear sung note held 1.5 beats or more, begun off
                        the beat, gets a note, in place of the beat it anticipates (on real songs, a
                        looser rule put half of these where no mapper had a note) */
        int m = 0; /* the tune's median strength: only its clear notes */
        for (int i = 0; i < ns; ++i)
            if (s[i].tune > 0) tmp[m++] = s[i].tune;
        float clear = m ? median_of(tmp, m) : 1e9f;
        for (int i = 0; i < ns; ++i) {
            if (s[i].keep || s[i].cls == 0 || s[i].tune < clear || s[i].p <= 0 || s[i].sus < maxf(1.5f * beat_len, 0.6f)) continue;
            if (from && i / bar < nbar && from[i / bar] >= 0) continue; /* a repeat follows its model, below */
            if (s[i].t < 0.15f || s[i].t > a->duration - 0.8f) continue;
            int clash = 0;
            for (int j = i - 1; j >= 0 && s[i].t - s[j].t < P[diff].gap; --j) clash |= s[j].keep && s[j].hold > 0;
            if (clash) continue;
            /* it anticipates the beat after it, which goes; the one before stays a quarter second off */
            for (int j = i - 1; j >= 0 && s[i].t - s[j].t < 0.25f; --j) s[j].keep = 0;
            for (int j = i + 1; j < ns && s[j].t - s[i].t < maxf(P[diff].gap, 0.6f * beat_len); ++j) s[j].keep = 0;
            s[i].keep = 1, s[i].chord = 0;
        }
        for (int b = 1; from && b < nbar; ++b) /* the repeats again, as their models now are */
            if (from[b] >= 0)
                for (int j = 0; j < bar; ++j) {
                    Slot *x = &s[b * bar + j], *y = &s[from[b] * bar + j];
                    int inside = x->t > 0.15f && x->t < a->duration - 0.8f && x->p > 0;
                    if (x->by_lead && x->keep) continue; /* the lead's own notes stay */
                    x->keep = y->keep && inside, x->chord = y->chord && inside;
                }
    }
    {
        /* the song's rises, summed hop by hop, and their median */
        float *hsum = calloc((size_t)a->ncurve + 1, sizeof *hsum), *hv = malloc((size_t)a->ncurve * sizeof *hv);
        float per = hop_seconds(1) - hop_seconds(0), med = 1;
        int m = 0, kept = 0;
        if (hsum && hv) {
            for (int h = 0; h < a->ncurve; ++h) {
                float v = 0;
                for (int j = 0; j < 6; ++j) v += a->curve[h * CURVES + j];
                hsum[h + 1] = hsum[h] + v, hv[h] = v;
            }
            med = maxf(median_of(hv, a->ncurve), 1);
        } else {
            free(hsum), hsum = 0;
        }
        free(hv);
        float least = maxf(0.3f, 0.75f * beat_len), half = 0.5f * beat_len;
        for (int i = 0; i < ns; ++i) {
            if (!s[i].keep) continue;
            ++kept;
            if (s[i].sus < least) continue;
            float quiet = 0;
            if (hsum) {
                int h0 = clampi((int)((s[i].t + 0.08f - hop_seconds(0)) / per), 0, a->ncurve - 1);
                int h1 = clampi((int)((s[i].t + beat_len - hop_seconds(0)) / per), h0 + 1, a->ncurve);
                quiet = clampf(1 - (hsum[h1] - hsum[h0]) / (h1 - h0) / (2 * med), 0, 1);
            }
            tmp[i] = minf(s[i].sus / beat_len, 4) * (s[i].by_level > 0 ? s[i].by_level : s[i].tune > 0 ? 1 : 0.6f) + quiet;
            idx[m++] = i;
        }
        for (int x = 0; x < m; ++x) /* best first (an insertion sort: a few hundred at most) */
            for (int y = x; y > 0 && tmp[idx[y]] > tmp[idx[y - 1]]; --y) {
                int t = idx[y];
                idx[y] = idx[y - 1], idx[y - 1] = t;
            }
        int most = (int)(P[diff].held * kept + 0.5f);
        for (int x = 0; x < m && holds < most; ++x) {
            int i = idx[x];
            if (!s[i].keep || s[i].under || s[i].hold > 0) continue;
            if (from && i / bar < nbar && from[i / bar] >= 0) continue; /* a repeat takes its model's, below */
            float len = minf(s[i].sus, 3.0f), end_at = a->duration - 0.5f;
            if (s[i].t + len > end_at) len = end_at - s[i].t;
            holds += place_hold(s, ns, i, len, half, diff, tmp[i] >= (s[i].tune > 0 ? 1.5f : 2.0f), 0);
        }
        /* a bar like an earlier one holds what that bar holds, where the note is held here too */
        for (int b = 1; from && b < nbar; ++b) {
            if (from[b] < 0) continue;
            for (int j = 0; j < bar; ++j) {
                Slot *e = &s[from[b] * bar + j];
                int i = b * bar + j;
                if (e->hold > 0 && s[i].keep && !s[i].under && s[i].hold <= 0 && s[i].chord == e->chord &&
                    s[i].sus >= 0.5f * least && holds < most) /* where it holds here too */
                    holds += place_hold(s, ns, i, e->hold, half, diff, e->strong, 0);
            }
        }
        /* then the best of the rest, anywhere; in a repeat, only up to the next note, so that its
         * rhythm stays its model's */
        for (int x = 0; x < m && holds < most; ++x) {
            int i = idx[x];
            if (!s[i].keep || s[i].under || s[i].hold > 0) continue;
            float len = minf(s[i].sus, 3.0f), end_at = a->duration - 0.5f;
            if (s[i].t + len > end_at) len = end_at - s[i].t;
            int rep = diff <= 1 && from && i / bar < nbar && from[i / bar] >= 0;
            holds += place_hold(s, ns, i, len, half, diff, tmp[i] >= (s[i].tune > 0 ? 1.5f : 2.0f), rep);
        }
        free(hsum);
    }
    /* 5b. no second busier than the difficulty allows: the weakest go, never a held note */
    {
        int most = (int)P[diff].peak;
        for (int i = 0; i < ns; ++i) {
            if (!s[i].keep) continue;
            for (;;) {
                int c = 0, weak = -1;
                for (int j = i; j < ns && s[j].t < s[i].t + 1.0f; ++j) {
                    if (!s[j].keep) continue;
                    ++c;
                    if (s[j].hold <= 0 && j != i && (weak < 0 || s[j].cls > s[weak].cls ||
                                                     (s[j].cls == s[weak].cls && s[j].sel < s[weak].sel)))
                        weak = j;
                }
                if (c <= most || weak < 0) break;
                s[weak].keep = 0;
            }
        }
    }
    /* 6. The notes, and their lanes: a shape a bar. A fresh bar takes the shape (or its mirror) whose
     * moves best follow the tune's (up to the right, down to the left, a repeat in place), other
     * things equal the next in turn; a bar like an earlier one takes its shape, mirrored unless the
     * tune says otherwise. */
    Note *n = calloc((size_t)ns + 1, sizeof *n);
    int *shape = calloc((size_t)(nbar + 1), sizeof *shape), *mirror = calloc((size_t)(nbar + 1), sizeof *mirror);
    if (!n || !shape || !mirror) {
        free(s), free(f), free(tmp), free(idx), free(from), free(near), free(n), free(shape), free(mirror);
        return 0;
    }
    /* soft bars (the song's quietest third) play gently: steps and bounces, no zigzag across the
     * three buttons, no chords (as charters write softer sections) */
    int *soft = calloc((size_t)(nbar + 1), sizeof *soft);
    if (soft && nbar >= 6) {
        for (int b = 0; b < nbar; ++b) {
            float v = 0;
            for (int j = 0; j < bar; ++j) v += f[b * bar + j][6];
            tmp[b] = v / bar;
        }
        float *srt = malloc((size_t)nbar * sizeof *srt);
        if (srt) {
            memcpy(srt, tmp, (size_t)nbar * sizeof *srt);
            float q = median_of(srt, nbar) > 0 ? srt[nbar / 3] : 0; /* sorted by median_of */
            for (int b = 0; b < nbar; ++b) soft[b] = tmp[b] < q;
            free(srt);
        }
        for (int i = 0; i < ns; ++i)
            if (soft[i / bar < nbar ? i / bar : nbar - 1]) s[i].chord = 0;
    }
    int fresh = 0, contour = 0;
    for (int b = 0; b < nbar; ++b) {
        int base_sh = from && from[b] >= 0 ? shape[from[b]] : (fresh * 2 + diff) % SHAPES;
        int base_mi = from && from[b] >= 0 ? !mirror[from[b]] : 0;
        int best_sh = base_sh, best_mi = base_mi, best = shape_fit(s, b * bar, (b + 1) * bar, base_sh, base_mi);
        if (soft && soft[b] && base_sh == 1) base_sh = 3, best = shape_fit(s, b * bar, (b + 1) * bar, base_sh, base_mi);
        for (int sh = 0; sh < SHAPES; ++sh)
            for (int mi = 0; mi < 2; ++mi) {
                if (soft && soft[b] && sh == 1) continue; /* no zigzag in a soft bar */
                if (from && from[b] >= 0 && sh != base_sh) continue; /* a repeat keeps its shape */
                int v = shape_fit(s, b * bar, (b + 1) * bar, sh, mi);
                if (v > best) best = v, best_sh = sh, best_mi = mi;
            }
        contour += best_sh != base_sh || best_mi != base_mi;
        if (!(from && from[b] >= 0)) ++fresh;
        shape[b] = best_sh, mirror[b] = best_mi;
    }
    /* a lane is free if its button wasn't pressed in the last dtap s (a chord presses both sides),
     * isn't pressed a third time within half a second of each press before, and isn't Centre too
     * soon after a side (Q2 Pod's chord gate) */
    int nn = 0, single_in_bar = 0, bar_now = -1, chords = 0, hold_lane = -1, prevmask = 0, held_mask = 0;
    float prev_t = -10, hold_end = -10, last[3] = { -10, -10, -10 }, before[3] = { -10, -10, -10 }; /* each button's presses */
#define AGAIN(L) (s[i].t - last[L] < P[diff].dtap || (s[i].t - last[L] < 0.5f && last[L] - before[L] < 0.5f))
#define FREE(L, dt) (!AGAIN(L) && \
                     !((L) == 1 && (prevmask & 5) && (dt) < P[diff].side_centre) && \
                     !((held_mask >> (L) & 1) && s[i].t < hold_end + P[diff].dtap))
    for (int i = 0; i < ns; ++i) {
        if (!s[i].keep) continue;
        float dt = s[i].t - prev_t;
        if (nn && dt < P[diff].gap * 0.9f) continue; /* across a bar's edge after a copy */
        int b = i / bar;
        if (b != bar_now) bar_now = b, single_in_bar = 0;
        int lanes = 0;
        if (s[i].under && hold_lane >= 0 && s[i].t < hold_end + 0.12f) {
            ++single_in_bar;
            int L = 2 - hold_lane; /* the other side, or nothing */
            if (FREE(L, dt)) lanes = 1 << L;
        } else if (s[i].chord && !AGAIN(0) && !AGAIN(2)) {
            lanes = 5;
        } else {
            const int8_t *sh = SHAPE[b < nbar ? shape[b] : 0];
            int len = 0;
            while (sh[len] >= 0) ++len;
            int want = sh[single_in_bar++ % len];
            if (b < nbar && mirror[b]) want = 2 - want;
            int order[3] = { want, 2 - want, 1 }; /* the shape's lane, its mirror, Centre */
            if (want == 1) order[1] = (single_in_bar & 1) ? 0 : 2, order[2] = 2 - order[1];
            for (int q = 0; q < 3 && !lanes; ++q) {
                int L = order[q];
                if (s[i].needs_side && L == 1) continue;
                if (FREE(L, dt)) lanes = 1 << L;
            }
        }
        if (!lanes) continue;
        n[nn].t = s[i].t, n[nn].lanes = (uint8_t)lanes;
        if (s[i].hold > 0 && lanes == 5) { /* a held chord: both sides, nothing inside */
            n[nn].len = s[i].hold, hold_lane = -1, hold_end = s[i].t + n[nn].len, held_mask = 5;
        } else if (s[i].hold > 0 && !(s[i].under && hold_lane >= 0)) {
            int lane = lanes == 1 ? 0 : lanes == 2 ? 1 : 2;
            n[nn].len = lane == 1 ? minf(s[i].hold, 1.5f) : s[i].hold;
            if (s[i].needs_side && lane == 1) n[nn].len = 0; /* never with notes inside */
            if (n[nn].len > 0) hold_lane = lane == 1 ? -1 : lane, hold_end = s[i].t + n[nn].len, held_mask = lanes;
        }
        chords += lanes == 5;
        for (int L = 0; L < 3; ++L)
            if (lanes >> L & 1) before[L] = last[L], last[L] = s[i].t;
        prevmask = lanes, prev_t = s[i].t;
        ++nn;
    }
#undef FREE
#undef AGAIN
    /* what came out: every note under a held one on the other side, nothing else inside one */
    holds = 0;
    for (int i = 0; i < nn; ++i) {
        if (n[i].len <= 0) continue;
        int bad = 0, l = n[i].lanes;
        for (int j = i + 1; j < nn && n[j].t < n[i].t + n[i].len + 0.1f; ++j)
            bad |= l == 2 || l == 5 || n[j].lanes == 5 || n[j].lanes == l || n[j].lanes == 2;
        if (bad) { /* cut it to what is clear */
            float to = 0;
            for (int j = i + 1; j < nn; ++j)
                if (n[j].lanes == 5 || n[j].lanes == l || n[j].lanes == 2 || l == 2 || l == 5) { to = n[j].t; break; }
            float len = to > 0 ? to - n[i].t - 0.15f : n[i].len;
            n[i].len = len >= 0.3f ? minf(n[i].len, len) : 0;
        }
        holds += n[i].len > 0;
    }
    /* Hero phrases: from 8 s, every 25 s or so, notes over about 3 s (four at least) */
    float at = 8;
    for (int i = 0; i < nn; ++i) {
        if (n[i].t < at) continue;
        int j = i;
        while (j < nn && (n[j].t < n[i].t + 3.2f || j - i < 4)) n[j++].hero = 1;
        n[j - 1].phrase = 1;
        at = n[j - 1].t + 22 + (float)(j % 7); /* the same chart each time */
        i = j - 1;
    }
    int base = 0;
    for (int i = 0; i < nn; ++i) base += (n[i].lanes == 5 ? 100 : 50) + (int)(n[i].len * 25);
    int copied = 0;
    for (int b = 0; b < nbar && from; ++b) copied += from[b] >= 0;
    free(s), free(f), free(tmp), free(idx), free(from), free(near), free(shape), free(mirror), free(soft);
    c->n = n, c->count = nn, c->window = P[diff].window, c->base_score = base;
    logf_("chart: %s: %d notes (%.2f a second), %d chords, %d held; %d of %d bars like an earlier one, %d shaped by the tune",
          diff_name[diff], nn, nn / maxf(a->duration, 1), chords, holds, copied, nbar, contour);
    return 1;
}

float chart_miss_drain(int diff) { return P[clampi(diff, 0, DIFFS - 1)].miss_drain; }

void chart_free(Chart *c) {
    free(c->n);
    memset(c, 0, sizeof *c);
}
