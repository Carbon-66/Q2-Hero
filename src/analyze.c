/* Listening to a song, once, as it is decoded (mono, 11025 Hz), to hear its parts apart: the
 * tune, the bass and the drums.
 *
 *   1. Every 128 samples (11.6 ms), a 512-point spectrum (two frames to one complex FFT), kept
 *      a moment as log levels. Each is split into what holds (its level is the median over
 *      120 ms: notes) and what strikes (its level is the median across 230 Hz: drums), the
 *      split of Fitzgerald's median filtering (2010). The rise of the held part in the bass and
 *      the middle are where notes start; the rise of the struck part, low, middle and high, are
 *      the kick, the snare and the hats.
 *   2. Every 46 ms, a 2048-point spectrum, flattened so only its peaks are left, gives the pitch
 *      that sounds most strongly in the tune's range (110 Hz to 1.1 kHz) and in the bass's (41 to
 *      131 Hz): each candidate scored by the sum of its harmonics, then an octave up if that
 *      explains it as well.
 *   3. The tune and the bass become notes: a run of frames at one pitch, started where its held
 *      part rises, and split again where it rises without a change of pitch (a repeated note).
 *   4. The tempo, from the whole onset curve's autocorrelation between 60 and 200 BPM leaning
 *      toward 120, and the beats, by dynamic programming over it (Ellis 2007).
 *
 * chart.c makes each difficulty's notes from those parts. The results are kept on the card
 * (charts/<hash>.bin), so a song is listened to once. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hero.h"

#define N 512
#define HOP 128
#define BINS (N / 2 + 1)
#define NB 2048           /* the pitch spectrum */
#define BBINS 720         /* its bins kept: to 3.9 kHz */
#define PHOP 4            /* hops a pitch frame */
#define PK_BINS 400       /* partials tracked to 2.15 kHz */
#define PK_LOOK 88        /* and for 4.1 s at most (pitch frames) */
#define PK_RING 92
#define LAG 5             /* the held part's median: 2 LAG + 1 hops */
#define MW (2 * LAG + 1)
#define FW 5              /* the struck part's median: 2 FW + 1 bins */
#define HOPS_PER_S ((float)AN_RATE / HOP)
#define MAX_HOPS (int)(HOPS_PER_S * 60 * 40) /* 40 minutes */
/* s: a hit is placed this much before the frame's centre. A real instrument's attack takes 20-30 ms
 * to reach the peak the rises find, and players and charters hear the note at the attack's start:
 * against eleven hand-made osu!mania maps the rises came 22 ms after the maps' grids, and the notes
 * on the beats found from them 7 ms after the maps' notes (with synthetic, instant attacks they
 * come early). */
#define ONSET_LAG 0.0245f
#define CACHE_MAGIC 0x43483251u /* "Q2HC" */
#define CACHE_VERSION 9
#define MEL_LO 45 /* the tune's candidates: A2 .. */
#define MEL_N 126 /* .. in thirds of a semitone */
#define BASS_LO 28 /* the bass's: E1 .. */
#define BASS_N 61 /* .. to C3 */
#define DF ((float)AN_RATE / NB)

enum { HB_BASS, HB_MID, HB_HIGH };
enum { PB_KICK, PB_SNARE, PB_HAT };
static const int hb_lo[3] = { 1, 10, 93 }, hb_hi[3] = { 10, 93, 232 };
static const int pb_lo[3] = { 1, 7, 116 }, pb_hi[3] = { 7, 60, 256 };

typedef struct {
    float hf[3]; /* the held part's rise: bass, middle, high */
    float pf[3]; /* the struck part's rise: kick, snare, hats */
    float tf;    /* the whole middle's rise, held and struck: a note struck again */
    float e;     /* log loudness */
} Hop;

typedef struct {
    float mel, mel_s;   /* the tune's strongest pitch (MIDI) and how strongly it sounds */
    float bass, bass_s;
    uint8_t ms[MEL_N], bs[BASS_N]; /* every candidate's strength, x 6, so a note can be followed
                                      while something else is louder */
} PFrame;

struct Listener {
    int16_t win[NB];  /* the last NB samples, circular: left and right together */
    int16_t side[NB]; /* and their difference, (left - right) / 2 */
    int at;
    long fill, frames; /* samples so far, small frames taken */
    int since;
    float frame[2][N];
    int pending;
    uint8_t spec[MW][BINS]; /* the last MW spectra, log levels */
    uint8_t tsort[BINS][MW]; /* each bin's last MW levels, kept sorted */
    float ering[MW];
    long nspec;
    float hprev[BINS], pprev[BINS], sprev[BINS];
    Hop *h;
    int nh, cap;
    PFrame *p;
    int np, pcap;
    float meter[5];
    uint8_t pk[PK_RING][PK_BINS]; /* the last pitch frames' spectra (the sum's, unweighted), log levels */
    uint8_t *psus;                 /* each pitch frame: how many frames its longest new partial lasts */
    int npsus, pscap;
};

static float hann[N], hannb[NB], cos_t[NB / 2], sin_t[NB / 2];
static float mel_bin[MEL_N], bass_bin[BASS_N];
static int tables;

static float mtof(float m) { return 440 * fexp((m - 69) * 0.05776227f); }

static void make_tables(void) {
    if (tables) return;
    for (int i = 0; i < N; ++i) hann[i] = 0.5f - 0.5f * fcos(TAU * i / N);
    for (int i = 0; i < NB; ++i) hannb[i] = 0.5f - 0.5f * fcos(TAU * i / NB);
    for (int i = 0; i < NB / 2; ++i) cos_t[i] = fcos(TAU * i / NB), sin_t[i] = -fsin(TAU * i / NB);
    for (int j = 0; j < MEL_N; ++j) mel_bin[j] = mtof(MEL_LO + j / 3.0f) / DF;
    for (int j = 0; j < BASS_N; ++j) bass_bin[j] = mtof(BASS_LO + j / 3.0f) / DF;
    tables = 1;
}

/* in place, radix 2, n a power of two up to NB */
static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j], re[j] = t;
            t = im[i], im[i] = im[j], im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = NB / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k) {
                float wr = cos_t[k * step], wi = sin_t[k * step];
                float *ar = re + i + k, *ai = im + i + k, *br = re + i + k + len / 2, *bi = im + i + k + len / 2;
                float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;
                *br = *ar - tr, *bi = *ai - ti;
                *ar += tr, *ai += ti;
            }
    }
}

/* a sorted window of n: old out, new in */
static void slide(uint8_t *v, int n, uint8_t old, uint8_t in) {
    int i = 0;
    while (i < n - 1 && v[i] != old) ++i;
    while (i > 0 && v[i - 1] > in) v[i] = v[i - 1], --i;
    while (i < n - 1 && v[i + 1] < in) v[i] = v[i + 1], ++i;
    v[i] = in;
}

static uint8_t median_u8(uint8_t *v, int n) {
    for (int i = 1; i < n; ++i) {
        uint8_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) v[j + 1] = v[j], --j;
        v[j + 1] = x;
    }
    return v[n / 2];
}

/* ---------------------------------------------------------------------------------------------
 * 1. The split, for spectrum c, once LAG more have come (or at the end, with what there is) */
static void split_hop(Listener *l, long c) {
    if (l->nh >= MAX_HOPS) return;
    if (l->nh == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8192;
        Hop *h = realloc(l->h, (size_t)cap * sizeof *h);
        if (!h) return;
        l->h = h, l->cap = cap;
    }
    long lo = c - LAG < 0 ? 0 : c - LAG, hi = c + LAG > l->nspec - 1 ? l->nspec - 1 : c + LAG;
    int cnt = (int)(hi - lo + 1);
    const uint8_t *s = l->spec[c % MW];
    uint8_t tm[BINS], fm[BINS], v[MW];
    if (cnt == MW && l->nspec > MW) /* the running sorted windows (kept up by spectrum_in) */
        for (int k = 0; k < BINS; ++k) tm[k] = l->tsort[k][LAG];
    else
        for (int k = 0; k < BINS; ++k) {
            for (int j = 0; j < cnt; ++j) v[j] = l->spec[(lo + j) % MW][k];
            tm[k] = median_u8(v, cnt);
        }
    { /* across the bins, a sorted window slid along */
        uint8_t w[2 * FW + 1];
        int m = 0;
        for (int q = 0; q <= FW; ++q) w[m++] = s[q];
        median_u8(w, m);
        for (int k = 0; k < BINS; ++k) {
            int a = k - FW, b = k + FW;
            if (k > 0) {
                if (b < BINS && a - 1 >= 0) slide(w, m, s[a - 1], s[b]);
                else if (b < BINS) { /* growing at the start */
                    int i = m++;
                    while (i > 0 && w[i - 1] > s[b]) w[i] = w[i - 1], --i;
                    w[i] = s[b];
                } else if (a - 1 >= 0) { /* shrinking at the end */
                    int i = 0;
                    while (w[i] != s[a - 1]) ++i;
                    for (; i < m - 1; ++i) w[i] = w[i + 1];
                    --m;
                }
            }
            fm[k] = w[m / 2];
        }
    }
    float H[BINS], P[BINS];
    for (int k = 0; k < BINS; ++k) {
        float t = tm[k], f = fm[k], mask = t * t / (t * t + f * f + 1);
        H[k] = s[k] * mask, P[k] = s[k] - H[k];
    }
    Hop *o = &l->h[l->nh++];
    for (int b = 0; b < 3; ++b) {
        float sh = 0, sp = 0;
        for (int k = hb_lo[b]; k < hb_hi[b]; ++k) {
            float d = H[k] - l->hprev[k];
            if (d > 0) sh += d;
        }
        for (int k = pb_lo[b]; k < pb_hi[b]; ++k) {
            float d = P[k] - l->pprev[k];
            if (d > 0) sp += d;
        }
        o->hf[b] = sh / (float)(hb_hi[b] - hb_lo[b]);
        o->pf[b] = sp / (float)(pb_hi[b] - pb_lo[b]);
    }
    float st = 0;
    for (int k = hb_lo[HB_MID]; k < hb_hi[HB_MID]; ++k) {
        float d = s[k] - l->sprev[k];
        if (d > 0) st += d;
        l->sprev[k] = s[k];
    }
    o->tf = st / (float)(hb_hi[HB_MID] - hb_lo[HB_MID]);
    o->e = l->ering[c % MW];
    memcpy(l->hprev, H, sizeof H);
    memcpy(l->pprev, P, sizeof P);
}

static void spectrum_in(Listener *l, const float *mag, float energy) {
    uint8_t *s = l->spec[l->nspec % MW];
    int full = l->nspec >= MW; /* s still holds the level leaving the window */
    for (int k = 0; k < BINS; ++k) {
        int q = (int)(flog(1 + 20 * mag[k]) * 30);
        uint8_t in = (uint8_t)(q > 255 ? 255 : q);
        if (full) slide(l->tsort[k], MW, s[k], in);
        else { /* filling: insert */
            uint8_t *v = l->tsort[k];
            int i = (int)l->nspec;
            while (i > 0 && v[i - 1] > in) v[i] = v[i - 1], --i;
            v[i] = in;
        }
        s[k] = in;
    }
    l->ering[l->nspec % MW] = flog(1e-3f + energy);
    static const int mb[6] = { 1, 7, 19, 47, 117, 256 };
    for (int b = 0; b < 5; ++b) {
        float m = 0;
        for (int k = mb[b]; k < mb[b + 1]; ++k) m += s[k];
        m /= (float)(mb[b + 1] - mb[b]) * 255;
        l->meter[b] += (m * 1.6f - l->meter[b]) * 0.3f;
    }
    ++l->nspec;
    if (l->nspec > LAG) split_hop(l, l->nspec - 1 - LAG);
}

/* the two small frames waiting, as one complex FFT: X = FFT(a + ib), A = (X[k] + X*[N-k]) / 2, B = (X[k] - X*[N-k]) / 2i */
static void flush(Listener *l) {
    float re[N], im[N], ma[BINS], mb[BINS], ea = 0, eb = 0;
    for (int i = 0; i < N; ++i) {
        re[i] = l->frame[0][i] * hann[i], im[i] = l->pending > 1 ? l->frame[1][i] * hann[i] : 0;
        ea += l->frame[0][i] * l->frame[0][i];
        if (l->pending > 1) eb += l->frame[1][i] * l->frame[1][i];
    }
    fft(re, im, N);
    for (int k = 0; k <= N / 2; ++k) {
        int j = (N - k) % N;
        float ar = (re[k] + re[j]) * 0.5f, ai = (im[k] - im[j]) * 0.5f;
        float br = (im[k] + im[j]) * 0.5f, bi = (re[j] - re[k]) * 0.5f;
        ma[k] = fsqrt(ar * ar + ai * ai), mb[k] = fsqrt(br * br + bi * bi);
    }
    spectrum_in(l, ma, ea / N);
    if (l->pending > 1) spectrum_in(l, mb, eb / N);
    l->pending = 0;
}

/* ---------------------------------------------------------------------------------------------
 * 2. Pitch: the flattened 2048-point spectrum's peaks, summed along each candidate's harmonics */
/* the strongest peak within tol bins of bin: a sung note's vibrato spreads its harmonics */
static float peak_at(const float *w, float bin, float tol) {
    int a = (int)(bin - tol), b = (int)(bin + tol + 1);
    if (a < 1) a = 1;
    if (b > BBINS - 1) b = BBINS - 1;
    float m = 0;
    for (int i = a; i <= b; ++i) m = maxf(m, w[i]);
    return m;
}

/* the sum of a pitch's harmonics; for the tune, times how rich in harmonics it is (a voice or
 * a lead is, a soft pad isn't) and a little more the higher it is (a tune sits above its chords) */
static float salience(const float *w, float bin, int harmonics, float top, float first, float rich) {
    float s = 0, upper = 0, g = first;
    for (int h = 1; h <= harmonics && bin * h < top; ++h, g *= h == 2 ? 0.84f / first : 0.84f) {
        float v = g * peak_at(w, bin * h, maxf(0.6f, 0.012f * bin * h));
        s += v;
        if (h > 1) upper += v;
    }
    if (rich > 0 && s > 0) s *= (0.45f + minf(upper / s, 0.55f)) * (1 + rich * flog(bin * DF / 110) * 1.442695f);
    return s;
}

static void best_pitch(const float *w, const float *bins, int n, int lo, int harmonics, float top, float first,
                       float rich, float *pitch, float *sal, uint8_t *keep) {
    int arg = 0;
    float best = 0, s[MEL_N];
    for (int j = 0; j < n; ++j) {
        s[j] = salience(w, bins[j], harmonics, top, first, rich);
        keep[j] = (uint8_t)clampi((int)(s[j] * 6), 0, 255);
        if (s[j] > best) best = s[j], arg = j;
    }
    /* summing harmonics favours an octave down: up an octave if that explains it nearly as well */
    if (arg + 36 < n && s[arg + 36] >= 0.72f * best && peak_at(w, bins[arg], 0.6f) < 0.6f * peak_at(w, bins[arg] * 2, 0.6f))
        arg += 36, best = s[arg];
    /* the third of a semitone either side that scores best, and between them */
    float y0 = arg > 0 ? s[arg - 1] : 0, y1 = s[arg], y2 = arg + 1 < n ? s[arg + 1] : 0, den = y0 - 2 * y1 + y2;
    float off = den < -1e-6f ? clampf(0.5f * (y0 - y2) / den, -0.5f, 0.5f) : 0;
    *pitch = lo + (arg + off) / 3.0f, *sal = best;
}

/* The tune is heard in what is panned to the centre (a voice, a lead) over what is spread to the
 * sides (guitars, keys, reverb): a bin whose difference is r of its sum keeps 1 - 2r of it (a tenth
 * at least). On mixes with a known tune (a sung line, formants and vibrato and glides, against
 * plucked arpeggios and strums in its own range panned left and right), the tune's notes found
 * right went from 36% to 73%. A mono song has no difference and is heard as before. */
#define CENTRE 2.0f

static void partials(Listener *l, int f);
static void pitch_from(Listener *l, const float *re, const float *im) {
    if (l->np == l->pcap) {
        int cap = l->pcap ? l->pcap * 2 : 2048;
        PFrame *p = realloc(l->p, (size_t)cap * sizeof *p);
        if (!p) return;
        l->p = p, l->pcap = cap;
    }
    float L[BBINS], w[BBINS], c[BBINS + 1];
    for (int k = 0; k < BBINS; ++k) { /* the pair: M = (X[k] + X*[N-k]) / 2, S = (X[k] - X*[N-k]) / 2i */
        int j = (NB - k) % NB;
        float mr = (re[k] + re[j]) * 0.5f, mi = (im[k] - im[j]) * 0.5f;
        float sr = (im[k] + im[j]) * 0.5f, si = (re[j] - re[k]) * 0.5f;
        float m = fsqrt(mr * mr + mi * mi), sd = fsqrt(sr * sr + si * si);
        float g = m > 1e-9f ? clampf(1 - CENTRE * sd / m, 0.1f, 1) : 1;
        L[k] = flog(1 + 20 * m * g);
        if (k < PK_BINS) l->pk[l->np % PK_RING][k] = (uint8_t)clampi((int)(flog(1 + 20 * m) * 30), 0, 255);
    }
    c[0] = 0;
    for (int k = 0; k < BBINS; ++k) c[k + 1] = c[k] + L[k];
    for (int k = 0; k < BBINS; ++k) { /* flattened: less the mean 65 Hz either side, peaks only */
        int a = k - 12 < 0 ? 0 : k - 12, b = k + 13 > BBINS ? BBINS : k + 13;
        float mean = (c[b] - c[a]) / (b - a);
        w[k] = L[k] - mean - 0.2f;
        if (w[k] < 0 || (k > 0 && L[k] < L[k - 1]) || (k + 1 < BBINS && L[k] < L[k + 1])) w[k] = maxf(0, w[k]) * 0.3f;
    }
    PFrame *o = &l->p[l->np++];
    best_pitch(w, mel_bin, MEL_N, MEL_LO, 8, 3000 / DF, 1.0f, 0.15f, &o->mel, &o->mel_s, o->ms);
    best_pitch(w, bass_bin, BASS_N, BASS_LO, 5, 900 / DF, 1.4f, 0, &o->bass, &o->bass_s, o->bs);
    if (l->np > PK_LOOK) partials(l, l->np - 1 - PK_LOOK);
}

/* How long a sound begun in pitch frame f holds: a voice's or strings' long note. A partial that is
 * new in frame f (a peak 7 dB over its neighbourhood, 3.5 dB over the same bins two frames before,
 * between 150 Hz and 2.1 kHz) lasts while its bin (or one either side, for vibrato) stays within
 * 6 dB of where it began. A held string chord under a moving voice keeps its partials; a plucked
 * or struck one lets them fall. The frame's value: how long its most prominent new partial lasts,
 * in frames. */
static void partials(Listener *l, int f) {
    if (l->npsus >= l->pscap) {
        int cap = l->pscap ? l->pscap * 2 : 2048;
        uint8_t *p = realloc(l->psus, (size_t)cap);
        if (!p) return;
        l->psus = p, l->pscap = cap;
    }
    while (l->npsus < f) l->psus[l->npsus++] = 0;
    const uint8_t *L = l->pk[f % PK_RING], *a = f >= 1 ? l->pk[(f - 1) % PK_RING] : 0, *b = f >= 2 ? l->pk[(f - 2) % PK_RING] : 0;
    int best = 0, top = 0;
    for (int k = 28; k < PK_BINS - 7 && a && b; ++k) {
        int v = L[k];
        if (v < 90 || v < L[k - 1] || v <= L[k + 1] || v < L[k - 2] || v < L[k + 2]) continue;
        int lo = 255;
        for (int d = -6; d <= 6; ++d) lo = L[k + d] < lo ? L[k + d] : lo;
        if (v - lo < 24) continue;
        int before = 0;
        for (int d = -1; d <= 1; ++d) {
            before = a[k + d] > before ? a[k + d] : before;
            before = b[k + d] > before ? b[k + d] : before;
        }
        if (before > v - 12) continue;
        int len = 0;
        for (int g = f + 1; g < l->np && g <= f + PK_LOOK; ++g, ++len) {
            const uint8_t *G = l->pk[g % PK_RING];
            int m = G[k - 1] > G[k] ? G[k - 1] : G[k];
            m = G[k + 1] > m ? G[k + 1] : m;
            if (m < v - 21) break;
        }
        if (v - lo > top) top = v - lo, best = len; /* the most prominent new partial's */
    }
    l->psus[l->npsus++] = (uint8_t)(best > 255 ? 255 : best);
}

/* a pitch frame: the sum as the real part and the difference as the imaginary, one complex FFT */
static void pitch_frame(Listener *l) {
    static float re[NB], im[NB];
    for (int i = 0; i < NB; ++i) {
        int k = (l->at + i) % NB;
        re[i] = l->win[k] * (1.0f / 32768) * hannb[i], im[i] = l->side[k] * (1.0f / 32768) * hannb[i];
    }
    fft(re, im, NB);
    pitch_from(l, re, im);
}

Listener *listen_new(void) {
    make_tables();
    return calloc(1, sizeof(Listener));
}

void listen_feed(Listener *l, const int16_t *pcm, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        int lft = pcm[2 * i], rgt = pcm[2 * i + 1];
        l->win[l->at] = (int16_t)((lft + rgt) >> 1), l->side[l->at] = (int16_t)((lft - rgt) >> 1);
        l->at = (l->at + 1) % NB;
        if (++l->fill < N) continue;
        if (l->fill > N && ++l->since < HOP) continue;
        l->since = 0;
        float *f = l->frame[l->pending++];
        for (int k = 0; k < N; ++k) f[k] = l->win[(l->at + NB - N + k) % NB] * (1.0f / 32768);
        if (l->frames++ % PHOP == 0) pitch_frame(l); /* frame t's pitch: centred at hop t - 6 */
        if (l->pending == 2) flush(l);
    }
}

float listen_meter(Listener *l, int band) { return l ? clampf(l->meter[band], 0, 1) : 0; }

/* ---------------------------------------------------------------------------------------------
 * After the last sample */

static int cmp_f(const void *a, const void *b) {
    float d = *(const float *)a - *(const float *)b;
    return d < 0 ? -1 : d > 0;
}
static float percentile(const float *v, int n, float q) {
    if (n <= 0) return 0;
    float *s = malloc((size_t)n * sizeof *s);
    if (!s) return v[0];
    memcpy(s, v, (size_t)n * sizeof *s);
    qsort(s, (size_t)n, sizeof *s, cmp_f);
    float r = s[clampi((int)(q * (n - 1)), 0, n - 1)];
    free(s);
    return r;
}

/* the moving mean of v over [i - r, i + r] */
static void moving_mean(const float *v, float *out, int n, int r) {
    double *c = malloc((size_t)(n + 1) * sizeof *c);
    if (!c) {
        memcpy(out, v, (size_t)n * sizeof *v);
        return;
    }
    c[0] = 0;
    for (int i = 0; i < n; ++i) c[i + 1] = c[i] + v[i];
    for (int i = 0; i < n; ++i) {
        int a = i - r < 0 ? 0 : i - r, b = i + r + 1 > n ? n : i + r + 1;
        out[i] = (float)((c[b] - c[a]) / (b - a));
    }
    free(c);
}

static float hop_time(float hop) { return (hop * HOP + N / 2) / AN_RATE - ONSET_LAG; }
float hop_seconds(int hop) { return hop_time((float)hop); }

/* a peak's place to a fraction of a hop */
static float refine(const float *v, int n, int i) {
    float y0 = i > 0 ? v[i - 1] : 0, y1 = v[i], y2 = i + 1 < n ? v[i + 1] : 0, den = y0 - 2 * y1 + y2;
    return i + (den < -1e-9f ? clampf(0.5f * (y0 - y2) / den, -0.5f, 0.5f) : 0);
}

typedef struct {
    Onset *v;
    int n, cap;
} Events;

static Onset *ev_add(Events *e) {
    if (e->n == e->cap) {
        Onset *m = realloc(e->v, (size_t)(e->cap = e->cap ? e->cap * 2 : 1024) * sizeof *m);
        if (!m) return 0;
        e->v = m;
    }
    memset(&e->v[e->n], 0, sizeof *e->v);
    return &e->v[e->n++];
}

/* peaks of curve over its moving mean: local maxima over +-3 hops, at least gap hops apart */
/* atan2, to a few thousandths of a radian (a phase: well under a ms of a beat) */
static float fatan2(float y, float x) {
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax < 1e-20f && ay < 1e-20f) return 0;
    float a = ax > ay ? ay / ax : ax / ay, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

/* One steady tempo for the whole song, as a charter sets it (one BPM, one offset), if the beats
 * found keep to it. The beats follow the drums one by one, and a syncopated stretch pulls them
 * off the beat by tens of ms; most songs are played to a click. Each tempo within 0.6% of the
 * beats' own is scored by how many beats fall within 12 ms of its grid (kernel), at its best
 * phase; the best is refined on the beats within 30 ms. It is used if 55% of the beats fall within
 * 30 ms of it, and 40% in each third (so that a tempo a hair off can't drift away). Against eleven
 * hand-made maps, notes more than 25 ms off the map's went from 11% to 6%; on the six songs taken
 * steady, to under 1%. */
static int steady_grid(float **beatp, int *nbp) {
    float *b = *beatp;
    int nb = *nbp;
    if (nb < 24) return 0;
    float *d = malloc((size_t)nb * sizeof *d);
    if (!d) return 0;
    for (int i = 1; i < nb; ++i) d[i - 1] = b[i] - b[i - 1];
    float T0 = percentile(d, nb - 1, 0.5f);
    free(d);
    double bestsc = -1, bT = T0, bt0 = 0;
    const double sig = 0.012;
    for (int q = -300; q <= 300; ++q) {
        double T = T0 * (1 + 0.006 * q / 300.0), cs = 0, sn = 0;
        for (int i = 0; i < nb; ++i) {
            double ph = 6.283185307 * (b[i] / T - (double)(long)(b[i] / T));
            cs += fcos((float)ph), sn += fsin((float)ph);
        }
        double ang = fatan2((float)sn, (float)cs), t0 = ang / 6.283185307 * T, sc = 0;
        for (int i = 0; i < nb; ++i) {
            double r = b[i] - t0, k = (double)(long)(r / T + (r >= 0 ? 0.5 : -0.5));
            r -= k * T;
            sc += fexp((float)(-0.5 * (r / sig) * (r / sig)));
        }
        if (sc > bestsc) bestsc = sc, bT = T, bt0 = t0;
    }
    double T = bT, t0 = bt0;
    for (int it = 0; it < 3; ++it) { /* refined: the phase, then a line through the beats within 30 ms */
        double sw = 0, swr = 0;
        for (int i = 0; i < nb; ++i) {
            double r = b[i] - t0, k = (double)(long)(r / T + (r >= 0 ? 0.5 : -0.5));
            r -= k * T;
            double w = fexp((float)(-0.5 * (r / sig) * (r / sig)));
            sw += w, swr += w * r;
        }
        if (sw > 0) t0 += swr / sw;
        double n = 0, sk = 0, st = 0, skk = 0, skt = 0;
        for (int i = 0; i < nb; ++i) {
            double k = (double)(long)((b[i] - t0) / T + ((b[i] - t0) >= 0 ? 0.5 : -0.5)), r = b[i] - (t0 + k * T);
            if (r > 0.03 || r < -0.03) continue;
            n += 1, sk += k, st += b[i], skk += k * k, skt += k * b[i];
        }
        double den = n * skk - sk * sk;
        if (n > 10 && den > 0) T = (n * skt - sk * st) / den, t0 = (st - T * sk) / n;
    }
    int in = 0, part[3] = { 0, 0, 0 }, cnt[3] = { 0, 0, 0 };
    for (int i = 0; i < nb; ++i) {
        double k = (double)(long)((b[i] - t0) / T + ((b[i] - t0) >= 0 ? 0.5 : -0.5)), r = b[i] - (t0 + k * T);
        int ok = r <= 0.03 && r >= -0.03, third = i * 3 / nb;
        in += ok, part[third] += ok, ++cnt[third];
    }
    int steady = in >= 0.55 * nb;
    for (int q = 0; q < 3; ++q) steady &= part[q] >= 0.4 * cnt[q];
    logf_("analysis: one steady %.3f BPM? %d%% of the beats keep to it (%d%%, %d%%, %d%% by thirds)%s", 60 / T, 100 * in / nb,
          100 * part[0] / cnt[0], 100 * part[1] / cnt[1], 100 * part[2] / cnt[2], steady ? ": yes" : "");
    if (!steady) return 0;
    long k0 = (long)((b[0] - t0) / T + ((b[0] - t0) >= 0 ? 0.5 : -0.5)), k1 = (long)((b[nb - 1] - t0) / T + 0.5);
    int n = (int)(k1 - k0 + 1);
    float *g = malloc((size_t)n * sizeof *g);
    if (!g || n < 8) {
        free(g);
        return 0;
    }
    for (int i = 0; i < n; ++i) g[i] = (float)(t0 + (k0 + i) * T);
    free(b);
    *beatp = g, *nbp = n;
    return 1;
}

/* the beats, by dynamic programming (Ellis 2007): the path through od's peaks that keeps nearest
 * to period hops apart; their times, evened out */
static float *track_beats(const float *od, int n, float period, int *count) {
    float sd = percentile(od, n, 0.97f), *beat = 0;
    int nb = 0;
    if (sd < 1e-4f) sd = 1e-4f;
    float *score = calloc((size_t)n, sizeof *score);
    int *back = calloc((size_t)n, sizeof *back);
    if (score && back) {
        const float tight = 20.0f;
        int lo = (int)(period * 0.5f), hi = (int)(period * 2);
        float pen[512];
        if (hi > 511) hi = 511;
        for (int d = lo; d <= hi; ++d) {
            float r = flog(d / period);
            pen[d] = tight * r * r;
        }
        for (int t = 0; t < n; ++t) {
            float m = -1e30f;
            int arg = -1;
            for (int prev = t - hi; prev <= t - lo; ++prev) {
                if (prev < 0) continue;
                float sc = score[prev] - pen[t - prev];
                if (sc > m) m = sc, arg = prev;
            }
            score[t] = od[t] / sd + (arg >= 0 ? m : 0);
            back[t] = arg;
        }
        int end = n - 1;
        float m = -1e30f;
        for (int t = n - 1; t >= 0 && t > n - 1 - (int)(period * 1.5f); --t)
            if (score[t] > m) m = score[t], end = t;
        for (int t = end; t >= 0; t = back[t]) ++nb;
        beat = malloc((size_t)(nb > 0 ? nb : 1) * sizeof *beat);
        int k = nb;
        for (int t = end; t >= 0 && beat; t = back[t]) beat[--k] = hop_time(refine(od, n, t));
        for (int pass = 0; pass < 2 && beat; ++pass) /* evened out: the hop-sized wobble taken out */
            for (int i = 1; i + 1 < nb; ++i) beat[i] = 0.5f * beat[i] + 0.25f * (beat[i - 1] + beat[i + 1]);
    }
    free(score), free(back);
    *count = beat ? nb : 0;
    return beat;
}

static int is_peak(const float *v, const float *mean, int n, int i, float floor_, float over) {
    if (i < 3 || i + 3 >= n || v[i] < floor_ || v[i] < over * mean[i]) return 0;
    for (int k = -3; k <= 3; ++k)
        if (k && v[i + k] > v[i]) return 0;
    return 1;
}

/* how strongly pitch p sounds in frame f: its candidate or a neighbour, whichever is stronger */
static float sal_at(const PFrame *f, int kind, float p) {
    int lo = kind ? BASS_LO : MEL_LO, n = kind ? BASS_N : MEL_N, j = (int)((p - lo) * 3 + 0.5f), m = 0;
    const uint8_t *v = kind ? f->bs : f->ms;
    for (int k = j - 1; k <= j + 1; ++k)
        if (k >= 0 && k < n && v[k] > m) m = v[k];
    return m / 6.0f;
}

/* A repeat of the pitch p at hop h shows in that pitch's own strength: it dips under 70% of where
 * it swells again. An attack in the accompaniment under a held note doesn't, and was splitting
 * sung notes in pieces: on the mixes with a known tune, notes found right went from 33% to 60% of
 * those found, and their lengths from 0.7 of the true ones to 1.0. */
static int own_dip(const PFrame *pf, int np, int kind, int h, float p) {
    const float dip_need = 0.7f;
    int f = (h + 6 + PHOP / 2) / PHOP;
    float pre = 1e9f, post = 0;
    for (int k = f - 2; k <= f - 1; ++k)
        if (k >= 0 && k < np) pre = minf(pre, sal_at(&pf[k], kind, p));
    for (int k = f; k <= f + 2; ++k)
        if (k >= 0 && k < np) post = maxf(post, sal_at(&pf[k], kind, p));
    return pre < dip_need * post;
}

/* the pitch a note between hops h0 and h1 sounds at: its frames' strengths summed, centres inside
 * the note; p0 (the run's pitch) unless another is clearly stronger there */
static float note_pitch(const PFrame *pf, int np, int kind, int h0, int h1, float p0) {
    int n = kind ? BASS_N : MEL_N, lo = kind ? BASS_LO : MEL_LO, f0 = (h0 + 6 + 2 + 3) / PHOP, f1 = (h1 + 6 - 1) / PHOP;
    if (f1 < f0) f1 = f0;
    float sum[MEL_N];
    memset(sum, 0, sizeof sum);
    for (int f = f0; f <= f1 && f < np; ++f) {
        const uint8_t *v = kind ? pf[f].bs : pf[f].ms;
        for (int j = 0; j < n; ++j) sum[j] += v[j];
    }
    int arg = 0;
    for (int j = 1; j < n; ++j)
        if (sum[j] > sum[arg]) arg = j;
    int at = clampi((int)((p0 - lo) * 3 + 0.5f), 0, n - 1);
    float here = sum[at];
    for (int k = at - 1; k <= at + 1; ++k)
        if (k >= 0 && k < n) here = maxf(here, sum[k]);
    if (abs(arg - at) <= 2 || sum[arg] < 1.15f * here) return p0;
    float y0 = arg > 0 ? sum[arg - 1] : 0, y1 = sum[arg], y2 = arg + 1 < n ? sum[arg + 1] : 0, den = y0 - 2 * y1 + y2;
    return lo + (arg + (den < -1e-6f ? clampf(0.5f * (y0 - y2) / den, -0.5f, 0.5f) : 0)) / 3.0f;
}

/* How far a tune note's pitch wavers about its own, semitones: the mean distance of each of its
 * frames' strongest pitch, where that is this note (within 1.5), from the note's. A voice slides
 * into a note and swells into vibrato; a piano, a guitar, a synth mostly hold still. */
static float wobble_of(const PFrame *pf, int np, int h0, int h1, float p) {
    int f0 = (h0 + 6 + 2) / PHOP, f1 = (h1 + 6 - 1) / PHOP, n = 0;
    float sum = 0;
    for (int f = f0; f <= f1 && f < np; ++f) {
        if (f < 0) continue;
        float d = fabs_(pf[f].mel - p);
        if (d < 1.5f) sum += d, ++n;
    }
    return n >= 2 ? sum / n : 0;
}

/* 3. A part's notes. One begins where the strongest pitch is clear and the held part rises near
 * it (a slowly swelling pad never does), and lasts while its own pitch still sounds, though
 * something else may be louder for a moment; a clearly louder new pitch ends it. A rise again on
 * the same pitch splits it: a repeated note. */
static void notes_of(Events *ev, int kind, const PFrame *pf, int np, const float *rise, const float *rmean, int nh,
                     float rnorm, const float *loud, const float *drum, const float *strike, const float *smean, float snorm2,
                     const float *any_drum) {
    float *sal = malloc((size_t)(np ? np : 1) * sizeof *sal), *pit = malloc((size_t)(np ? np : 1) * sizeof *pit);
    if (!sal || !pit) {
        free(sal), free(pit);
        return;
    }
    for (int i = 0; i < np; ++i) sal[i] = kind ? pf[i].bass_s : pf[i].mel_s, pit[i] = kind ? pf[i].bass : pf[i].mel;
    float voiced = 0.42f * percentile(sal, np, 0.9f);
    float snorm = percentile(sal, np, 0.95f);
    if (snorm < 1e-3f) snorm = 1e-3f;
    for (int i = 1; i + 1 < np; ++i) { /* a lone frame's octave slip: the median of three */
        float a = pit[i - 1], b = pit[i], c = pit[i + 1];
        float m = a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
        if (fabs_(b - m) > 0.75f && fabs_(a - c) < 0.75f) pit[i] = m;
    }
    int i = 0, prev_end = -10;
    while (i < np) {
        if (sal[i] < voiced) {
            ++i;
            continue;
        }
        float p0 = pit[i], s0 = sal[i], ssum = sal[i];
        int j = i + 1, quiet = 0, louder = 0;
        if (j < np && sal[j] >= voiced && fabs_(pit[j] - p0) < 0.8f) p0 = 0.5f * (pit[i] + pit[j]);
        while (j < np) {
            float sc = sal_at(&pf[j], kind, p0);
            /* another pitch clearly louder, two frames running (a kick drum's thump is one) */
            int other = sal[j] >= voiced && fabs_(pit[j] - p0) > 0.8f && sal[j] > 1.3f * sc;
            louder = other ? louder + 1 : 0;
            if (louder >= 2) {
                --j;
                break;
            }
            /* a bass note fades more than a sung one before it's over */
            if (sc >= maxf((kind ? 0.3f : 0.45f) * s0, (kind ? 0.5f : 0.7f) * voiced) || other) quiet = 0, ssum += sc;
            else if (++quiet > 1) break;
            ++j;
        }
        int end = j - quiet;
        /* in hops: frame f is centred at hop 4f - 6 */
        int h0 = i * PHOP - 6, h1 = end * PHOP - 6;
        int best = -1;
        float bv = 0;
        for (int h = h0 - 9; h <= h0 + 3; ++h)
            if (h > 0 && h < nh && rise[h] > bv && rise[h] >= rise[h - 1] && (h + 1 >= nh || rise[h] >= rise[h + 1])) bv = rise[h], best = h;
        int legato = i - prev_end <= 1; /* straight on from the last note: a slur has little attack */
        int soon = i - prev_end <= 3;   /* or within 140 ms */
        Onset *last = ev->n && ev->v[ev->n - 1].kind == kind ? &ev->v[ev->n - 1] : 0;
        /* the same pitch again straight after, begun only by a drum hit's leak: the same note */
        if (last && soon && fabs_(last->pitch - p0) < 0.8f) {
            int b = clampi(best, 0, nh - 1);
            float d = maxf(drum[b], b + 1 < nh ? drum[b + 1] : 0);
            if (best < 0 || bv < 0.45f * rnorm || (d > 0.45f && bv / rnorm < 1.3f * d) ||
                (!kind && !own_dip(pf, np, kind, b, p0))) {
                last->dur = maxf(last->dur, hop_time((float)h1) - last->t);
                prev_end = end;
                i = j;
                continue;
            }
        }
        if (best < 0 || bv < 0.2f * rnorm) {
            if (!legato) {
                i = j;
                continue;
            }
            best = h0 < 0 ? 0 : h0;
        }
        prev_end = end;
        int start = best;
        /* split at rises on the same pitch: a repeated note */
        int cut[64], ncut = 0;
        cut[ncut++] = start;
        for (int h = start + 6; h < h1 - 3 && h < nh && ncut < 63; ++h) {
            /* a repeat comes with an attack: half as strong as the note's own, at least; the tune's
             * repeats are found in the whole middle's rise, as the held part smooths a quick one over */
            int held = is_peak(rise, rmean, nh, h, (kind ? 0.5f : 0.35f) * rnorm, kind ? 2.2f : 1.6f) &&
                       rise[h] >= 0.5f * rise[clampi(start, 0, nh - 1)];
            /* or, for the tune, a strong strike with no drum at all near it (a quick repeat's pick) */
            int struck = !kind && is_peak(strike, smean, nh, h, 0.7f * snorm2, 1.5f) &&
                         maxf(any_drum[h], maxf(any_drum[h - 1], h + 1 < nh ? any_drum[h + 1] : 0)) < 0.3f;
            if ((!held && !struck) || h - cut[ncut - 1] < 6) continue;
            /* on a drum hit, some of the drum is heard as held: the note must rise more than it */
            float d = maxf(drum[h], maxf(drum[h - 1], h + 1 < nh ? drum[h + 1] : 0));
            float r = held ? rise[h] / rnorm : strike[h] / snorm2;
            if (d > 0.45f && r < 1.3f * d) continue;
            if (!kind && !own_dip(pf, np, kind, h, p0)) continue; /* the accompaniment's attack */
            cut[ncut++] = h;
        }
        cut[ncut] = h1 > start ? h1 : start + 4;
        float strength = clampf(ssum / (j - i) / snorm, 0, 1.3f) / 1.3f;
        for (int k = 0; k < ncut; ++k) {
            float dur = (cut[k + 1] - cut[k]) / HOPS_PER_S;
            if (dur < 0.06f) continue;
            Onset *o = ev_add(ev);
            if (!o) break;
            o->t = hop_time(refine(rise, nh, clampi(cut[k], 0, nh - 1)));
            o->dur = dur, o->kind = (uint8_t)kind;
            o->pitch = ncut > 1 ? note_pitch(pf, np, kind, cut[k], cut[k + 1], p0) : p0;
            o->strength = strength * (0.7f + 0.3f * clampf(rise[clampi(cut[k], 0, nh - 1)] / rnorm, 0, 1));
            o->local = loud[clampi(cut[k], 0, nh - 1)];
            o->wobble = kind ? 0 : wobble_of(pf, np, cut[k], cut[k + 1], o->pitch);
        }
        i = j;
    }
    free(sal), free(pit);
}

/* a tune note on a bass note's pitch or its octave, begun with it or while it sounds, is the bass */
static void drop_bass_echoes(Events *ev) {
    int out = 0;
    for (int i = 0; i < ev->n; ++i) {
        Onset *o = &ev->v[i];
        int echo = 0;
        if (o->kind == KIND_TUNE)
            for (int k = 0; k < ev->n && !echo; ++k) {
                const Onset *b = &ev->v[k];
                int with = fabs_(b->t - o->t) < 0.06f; /* begun with it: up to two octaves; while it sounds: one */
                if (b->kind != KIND_BASS || o->t < b->t - 0.06f || o->t > b->t + b->dur || o->pitch > b->pitch + (with ? 24.6f : 12.6f)) continue;
                float d = o->pitch - b->pitch;
                d -= 12 * (float)(int)((d + 6) / 12);
                echo = fabs_(d) < 0.6f && o->strength < b->strength; /* a riff doubling the bass is louder than it */
            }
        if (!echo) ev->v[out++] = *o;
    }
    ev->n = out;
}

/* a tune note an octave off its neighbours both sides, close to them an octave the other way, is
 * a slip (a riff doubling the bass reads an octave up): tunes rarely leap an octave and straight
 * back */
static void fix_octaves(Events *ev) {
    /* first, two notes of one part begun together are one: the stronger, the longer if level */
    int out = 0;
    for (int i = 0; i < ev->n; ++i) {
        Onset *o = &ev->v[i];
        int k = out - 1;
        while (k >= 0 && o->t - ev->v[k].t < 0.04f && ev->v[k].kind != o->kind) --k;
        if (k >= 0 && o->kind <= KIND_BASS && ev->v[k].kind == o->kind && o->t - ev->v[k].t < 0.04f) {
            Onset *p = &ev->v[k];
            if (o->strength > p->strength + 0.05f || (fabs_(o->strength - p->strength) <= 0.05f && o->dur > p->dur)) *p = *o;
            continue;
        }
        ev->v[out++] = *o;
    }
    ev->n = out;
    for (int i = 0; i < ev->n; ++i) {
        Onset *o = &ev->v[i];
        if (o->kind != KIND_TUNE) continue;
        const Onset *a = 0, *b = 0;
        for (int k = i - 1; k >= 0 && !a; --k)
            if (ev->v[k].kind == KIND_TUNE) a = &ev->v[k];
        for (int k = i + 1; k < ev->n && !b; ++k)
            if (ev->v[k].kind == KIND_TUNE) b = &ev->v[k];
        if (!a || !b || o->t - a->t > 0.8f || b->t - o->t > 0.8f) continue;
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            float q = o->pitch + 12 * sgn;
            if (fabs_(o->pitch - a->pitch) >= 9 && fabs_(o->pitch - b->pitch) >= 9 && fabs_(q - a->pitch) <= 7 &&
                fabs_(q - b->pitch) <= 7)
                o->pitch = q;
        }
    }
}

static int by_time(const void *a, const void *b) {
    float d = ((const Onset *)a)->t - ((const Onset *)b)->t;
    return d < 0 ? -1 : d > 0;
}

int listen_finish(Listener *l, Analysis *a) {
    if (l->pending) flush(l);
    for (long c = l->nspec - LAG < 0 ? 0 : l->nspec - LAG; c < l->nspec; ++c) split_hop(l, c);
    for (int f = l->np - PK_LOOK - 1 < 0 ? 0 : l->np - PK_LOOK - 1; f < l->np; ++f)
        if (f >= l->npsus) partials(l, f);
    int n = l->nh, np = l->np;
    Hop *h = l->h;
    if (n < 2 * (int)HOPS_PER_S || np < 4) {
        free(h), free(l->p), free(l->psus), free(l);
        return -1;
    }
    float *tmp = calloc((size_t)n, sizeof *tmp), *o = calloc((size_t)n, sizeof *o), *mean = calloc((size_t)n, sizeof *mean);
    float *od = calloc((size_t)n, sizeof *od), *loud = calloc((size_t)n, sizeof *loud);
    float *curve[6], *cmean[6], norm[6];
    int ok = tmp && o && mean && od && loud;
    for (int b = 0; b < 6; ++b) {
        curve[b] = calloc((size_t)n, sizeof(float)), cmean[b] = calloc((size_t)n, sizeof(float));
        ok &= curve[b] && cmean[b];
    }
    Events ev = { 0, 0, 0 };
    uint8_t *a_curve = 0;
    int swing = 0;
    float *beat = 0;
    int nb = 0;
    float period = HOPS_PER_S * 0.5f;
    if (!ok) goto done;

    /* the six rises, each to its own scale */
    for (int b = 0; b < 6; ++b) {
        for (int i = 0; i < n; ++i) curve[b][i] = b < 3 ? h[i].hf[b] : h[i].pf[b - 3];
        curve[b][0] = 0;
        norm[b] = percentile(curve[b], n, 0.97f);
        if (norm[b] < 1e-4f) norm[b] = 1e-4f;
        moving_mean(curve[b], cmean[b], n, (int)(HOPS_PER_S * 0.3f));
    }
    /* loudness, to the song's own scale */
    for (int i = 0; i < n; ++i) tmp[i] = h[i].e;
    float e_hi = percentile(tmp, n, 0.92f), e_lo = percentile(tmp, n, 0.08f);
    if (e_hi - e_lo < 1) e_lo = e_hi - 1;
    for (int i = 0; i < n; ++i) tmp[i] = (h[i].e - e_lo) / (e_hi - e_lo);
    moving_mean(tmp, loud, n, (int)(HOPS_PER_S * 1.5f));
    for (int i = 0; i < n; ++i) loud[i] = clampf(loud[i], 0, 1.5f);

    /* the drums: peaks of the struck rises */
    for (int b = 0; b < 3; ++b) {
        const float *v = curve[3 + b], *m = cmean[3 + b];
        int last = -100;
        for (int i = 0; i < n; ++i) {
            if (!is_peak(v, m, n, i, 0.3f * norm[3 + b], 1.8f) || i - last < 5) continue;
            last = i;
            Onset *x = ev_add(&ev);
            if (!x) break;
            x->t = hop_time(refine(v, n, i));
            x->kind = (uint8_t)(KIND_KICK + b);
            x->strength = clampf(v[i] / norm[3 + b], 0, 1.5f) / 1.5f;
            x->dur = 0.1f, x->local = loud[i];
            x->crash = b == PB_HAT && curve[3][i] > 0.6f * norm[3] && v[i] > 0.9f * norm[5];
        }
    }
    /* the tune and the bass; the drums each might be confused with (to their own scale) */
    for (int i = 0; i < n; ++i) o[i] = h[i].tf;
    float tnorm = maxf(percentile(o, n, 0.97f), 1e-4f);
    moving_mean(o, mean, n, (int)(HOPS_PER_S * 0.3f));
    for (int i = 0; i < n; ++i) tmp[i] = maxf(curve[3][i] / norm[3], curve[4][i] / norm[4]);
    for (int i = 0; i < n; ++i) od[i] = maxf(tmp[i], curve[5][i] / norm[5]); /* any drum, hats too */
    notes_of(&ev, KIND_TUNE, l->p, np, curve[HB_MID], cmean[HB_MID], n, norm[HB_MID], loud, tmp, o, mean, tnorm, od);
    /* a snare's body is low too */
    notes_of(&ev, KIND_BASS, l->p, np, curve[HB_BASS], cmean[HB_BASS], n, norm[HB_BASS], loud, tmp, o, mean, tnorm, od);
    qsort(ev.v, (size_t)ev.n, sizeof *ev.v, by_time);
    drop_bass_echoes(&ev);
    fix_octaves(&ev);

#ifdef HERO_HOST
    { /* the host tools' look inside: each hop's six rises (to their scale), loudness and pitch */
        const char *dump = getenv("HERO_DUMP");
        FILE *f = dump ? fopen(dump, "wb") : 0;
        for (int i = 0; f && i < n; ++i) {
            float row[10];
            for (int b = 0; b < 6; ++b) row[b] = curve[b][i] / norm[b];
            const PFrame *q = &l->p[clampi((i + 6) / PHOP, 0, np - 1)];
            row[6] = loud[i], row[7] = q->mel_s, row[8] = q->mel, row[9] = q->bass;
            fwrite(row, sizeof row, 1, f);
        }
        if (f) fclose(f);
    }
#endif
    /* 4. The tempo and the beats, from the drums. Measured against eleven hand-made osu!mania maps
     * (test/maps.py): the onsets of everything together put a strong syncopation's dotted beat
     * (2/3 of the tempo) first in five of them; the kick, snare and hats alone, with a comb that
     * also scores the beat's double and its bar (2x, 4x), found the tempo or half of it in all
     * eleven. The beats follow the kick and snare. */
    static const float tempo_w[6] = { 0, 0, 0, 1, 1, 0.6f }, beat_w[6] = { 0, 0, 0, 1, 1, 0 };
    for (int pass = 0; pass < 2; ++pass) {
        const float *w = pass ? beat_w : tempo_w;
        for (int i = 0; i < n; ++i) {
            float s = 0;
            for (int b = 3; b < 6; ++b) s += w[b] * curve[b][i] / norm[b];
            o[i] = s;
        }
        moving_mean(o, mean, n, (int)(HOPS_PER_S * 0.4f));
        for (int i = 0; i < n; ++i) od[i] = maxf(0, o[i] - mean[i]);
        if (pass) break;
        int maxlag = (int)(HOPS_PER_S * 4 + 6); /* 4 x a 60 BPM beat */
        float *ac = calloc((size_t)maxlag + 2, sizeof *ac);
        if (!ac) break;
        for (int lag = 1; lag <= maxlag; ++lag) {
            double sum = 0;
            for (int i = 0; i + lag < n; ++i) sum += (double)od[i] * od[i + lag];
            ac[lag] = (float)(sum / n);
        }
        float best = -1;
        for (float L = HOPS_PER_S * 60 / 240; L < HOPS_PER_S; L += 0.05f) {
            static const float k[3] = { 1, 2, 4 }, kw[3] = { 1, 0.6f, 0.4f };
            float sc = 0;
            for (int q = 0; q < 3; ++q) {
                float x = k[q] * L;
                int xi = (int)x;
                if (xi + 1 > maxlag) continue;
                sc += kw[q] * (ac[xi] + (x - xi) * (ac[xi + 1] - ac[xi]));
            }
            float bpm = HOPS_PER_S * 60 / L, d = flog(bpm / 110) * 1.442695f;
            sc *= fexp(-0.5f * d * d);
            if (sc > best) best = sc, period = L;
        }
        free(ac);
    }
    beat = track_beats(od, n, period, &nb);
    /* 5. What comes between the beats. A beat under 100 BPM with hits on its quarters is a half-time
     * feel, and the maps play it twice as fast (all six such maps); hits on its thirds and not its
     * quarters are a swing, charted on thirds. Measured (kick, snare and hats): quarters against
     * halves 0.65-1.08 in the maps, 0.18 in a shuffle; thirds against quarters 0.4-0.9 and 5.3. Quarters
     * no stronger than thirds are only the floor of the other instruments: the half-time maps had
     * 1.6-2.4 times as much on the quarters, a slow ballad 1.15. */
    if (beat && nb > 8) {
        for (int i = 0; i < n; ++i) {
            float sum = 0;
            for (int b = 3; b < 6; ++b) sum += tempo_w[b] * curve[b][i] / norm[b];
            o[i] = sum;
        }
        moving_mean(o, mean, n, (int)(HOPS_PER_S * 0.4f));
        for (int i = 0; i < n; ++i) od[i] = maxf(0, o[i] - mean[i]);
        static const float at[4] = { 0.5f, 0.25f, 0.75f, 1.0f / 3 };
        float e[5] = { 0, 0, 0, 0, 0 };
        for (int q = 0; q < 5; ++q) {
            float fr = q < 4 ? at[q] : 2.0f / 3;
            int k = 0;
            for (int j = 0; j + 1 < nb; ++j) {
                float t = beat[j] + (beat[j + 1] - beat[j]) * fr;
                int h = (int)(((t + ONSET_LAG) * AN_RATE - N / 2) / HOP + 0.5f);
                if (h < 2 || h + 2 >= n) continue;
                float m = 0;
                for (int d = -2; d <= 2; ++d) m = maxf(m, od[h + d]);
                e[q] += m, ++k;
            }
            if (k) e[q] /= k;
        }
        float half = e[0], quarters = 0.5f * (e[1] + e[2]), thirds = 0.5f * (e[3] + e[4]);
        if (HOPS_PER_S * 60 / period < 100 && quarters >= 0.4f * half && quarters >= 1.4f * thirds) { /* the beats again, twice as fast */
            for (int i = 0; i < n; ++i) {
                float sum = 0;
                for (int b = 3; b < 6; ++b) sum += beat_w[b] * curve[b][i] / norm[b];
                o[i] = sum;
            }
            moving_mean(o, mean, n, (int)(HOPS_PER_S * 0.4f));
            for (int i = 0; i < n; ++i) od[i] = maxf(0, o[i] - mean[i]);
            int nb2;
            float *b2 = track_beats(od, n, period * 0.5f, &nb2);
            if (b2 && nb2 > nb) free(beat), beat = b2, nb = nb2, period *= 0.5f;
            else free(b2);
        } else if (thirds >= 2 * quarters && thirds > 0.3f * half) {
            swing = 1;
        }
        logf_("analysis: between the beats: halves %.2f, quarters %.2f, thirds %.2f%s", half, quarters, thirds,
              swing ? ": a swing" : "");
    }
    if (beat && steady_grid(&beat, &nb)) period = (beat[nb - 1] - beat[0]) / (nb - 1) * HOPS_PER_S;
    /* each hop's six rises (to their scale, x 40) and loudness (x 150), for chart.c's slots */
    a_curve = malloc((size_t)n * CURVES);
    for (int i = 0; i < n && a_curve; ++i) {
        for (int b = 0; b < 6; ++b) a_curve[i * CURVES + b] = (uint8_t)clampi((int)(curve[b][i] / norm[b] * 40), 0, 255);
        a_curve[i * CURVES + 6] = (uint8_t)clampi((int)(loud[i] * 150), 0, 255);
        int pf = clampi((i + 6 + PHOP / 2) / PHOP, 0, l->npsus - 1);
        a_curve[i * CURVES + 7] = l->npsus ? l->psus[pf] : 0;
    }

done:
    memset(a, 0, sizeof *a);
    a->curve = a_curve, a->ncurve = a_curve ? n : 0;
    a->swing = swing;
    a->duration = n / HOPS_PER_S + (float)N / AN_RATE;
    a->bpm = HOPS_PER_S * 60 / period;
    a->beat = beat, a->nbeats = beat ? nb : 0;
    a->on = ev.v, a->nonsets = ev.n;
    a->nlevel = (int)(a->duration * 4) + 1;
    a->level = calloc((size_t)a->nlevel, sizeof *a->level);
    for (int i = 0; i < n && a->level && ok; ++i) {
        int k = (int)(i / HOPS_PER_S * 4);
        if (k < a->nlevel) a->level[k] = maxf(a->level[k], clampf(loud[i], 0, 1));
    }
    int count[KINDS] = { 0 };
    for (int i = 0; i < ev.n; ++i) ++count[ev.v[i].kind];
    logf_("analysis: %d hops, %.1f s, %.1f BPM, %d beats; %d tune notes, %d bass notes, %d kicks, %d snares, %d hats", n,
          a->duration, a->bpm, a->nbeats, count[KIND_TUNE], count[KIND_BASS], count[KIND_KICK], count[KIND_SNARE],
          count[KIND_HAT]);
    free(tmp), free(o), free(mean), free(od), free(loud);
    for (int b = 0; b < 6; ++b) free(curve[b]), free(cmean[b]);
    free(h), free(l->p), free(l->psus), free(l);
    return a->nbeats > 4 && a->nonsets > 4 ? 0 : -1;
}

void analysis_free(Analysis *a) {
    free(a->beat), free(a->on), free(a->level), free(a->curve);
    a->beat = 0, a->on = 0, a->level = 0, a->curve = 0;
    a->nbeats = a->nonsets = a->nlevel = a->ncurve = 0;
}

/* ---------------------------------------------------------------------------------------------
 * The cache: magic, version, the song's size, the info, the counts, then the arrays */

typedef struct {
    uint32_t magic, version;
    int32_t size, nbeats, nonsets, nlevel, ncurve, swing;
    float duration, bpm;
    double song_duration;
    char title[128], artist[128];
} CacheHead;

int analysis_save(const Analysis *a, const char *path, long size) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    CacheHead hd;
    memset(&hd, 0, sizeof hd);
    hd.magic = CACHE_MAGIC, hd.version = CACHE_VERSION, hd.size = (int32_t)size;
    hd.nbeats = a->nbeats, hd.nonsets = a->nonsets, hd.nlevel = a->nlevel, hd.ncurve = a->ncurve, hd.swing = a->swing;
    hd.duration = a->duration, hd.bpm = a->bpm, hd.song_duration = a->info.duration;
    memcpy(hd.title, a->info.title, sizeof hd.title);
    memcpy(hd.artist, a->info.artist, sizeof hd.artist);
    int ok = fwrite(&hd, sizeof hd, 1, f) == 1 &&
             fwrite(a->beat, sizeof *a->beat, (size_t)a->nbeats, f) == (size_t)a->nbeats &&
             fwrite(a->on, sizeof *a->on, (size_t)a->nonsets, f) == (size_t)a->nonsets &&
             fwrite(a->level, sizeof *a->level, (size_t)a->nlevel, f) == (size_t)a->nlevel &&
             fwrite(a->curve, CURVES, (size_t)a->ncurve, f) == (size_t)a->ncurve;
    ok &= !fclose(f);
    if (ok) ok = !rename(tmp, path);
    if (!ok) unlink(tmp);
    return ok;
}

int analysis_load(Analysis *a, const char *path, long size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    CacheHead hd;
    memset(a, 0, sizeof *a);
    int ok = fread(&hd, sizeof hd, 1, f) == 1 && hd.magic == CACHE_MAGIC && hd.version == CACHE_VERSION &&
             hd.size == (int32_t)size && hd.nbeats > 0 && hd.nbeats < 1000000 && hd.nonsets > 0 &&
             hd.nonsets < 2000000 && hd.nlevel > 0 && hd.nlevel < 100000 && hd.ncurve > 0 && hd.ncurve < MAX_HOPS + 10;
    if (ok) {
        a->beat = malloc((size_t)hd.nbeats * sizeof *a->beat);
        a->on = malloc((size_t)hd.nonsets * sizeof *a->on);
        a->level = malloc((size_t)hd.nlevel * sizeof *a->level);
        a->curve = malloc((size_t)hd.ncurve * CURVES);
        ok = a->beat && a->on && a->level && a->curve &&
             fread(a->beat, sizeof *a->beat, (size_t)hd.nbeats, f) == (size_t)hd.nbeats &&
             fread(a->on, sizeof *a->on, (size_t)hd.nonsets, f) == (size_t)hd.nonsets &&
             fread(a->level, sizeof *a->level, (size_t)hd.nlevel, f) == (size_t)hd.nlevel &&
             fread(a->curve, CURVES, (size_t)hd.ncurve, f) == (size_t)hd.ncurve;
    }
    fclose(f);
    if (!ok) {
        analysis_free(a);
        return 0;
    }
    a->nbeats = hd.nbeats, a->nonsets = hd.nonsets, a->nlevel = hd.nlevel, a->ncurve = hd.ncurve, a->swing = hd.swing != 0;
    a->duration = hd.duration, a->bpm = hd.bpm, a->info.duration = hd.song_duration;
    memcpy(a->info.title, hd.title, sizeof hd.title);
    memcpy(a->info.artist, hd.artist, sizeof hd.artist);
    a->info.title[sizeof a->info.title - 1] = a->info.artist[sizeof a->info.artist - 1] = 0;
    return 1;
}
