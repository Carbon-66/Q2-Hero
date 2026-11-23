/* Q2 Hero: a guitar game for the Shanling Q2 under Q2 Pod, played to the music on the card.
 *
 * A song is decoded by the Q2's own ffmpeg, listened to once (analyze.c: its beats, its hits and
 * how its pitch moves), and charted for three lanes, played on Prev, Centre and Next.
 *
 * Shared declarations for every file in src/. */
#ifndef HERO_H
#define HERO_H
#include <stdint.h>

#define VERSION "1.7"
#define VIEW_W 375 /* the UI's landscape view: the portrait panel turned */
#define VIEW_H 320
#define CORNER 80 /* the glass's rounded corners, px */

/* Q2 Pod's AWTK key codes */
#define K_RETURN 170
#define K_PLAY 171
#define K_CENTRE 218
#define K_PREV 222
#define K_NEXT 223

#define PI 3.14159265f
#define TAU 6.28318531f
#define RATE 44100 /* the output, and the music as played */
#define AN_RATE 11025 /* the music as listened to */
#define LANES 3

/* ---------------------------------------------------------------------------------------------
 * q2.c: the platform */
uint64_t now_us(void);
void sleep_us(unsigned us);
void present(const uint32_t *frame); /* a VIEW_W x VIEW_H XRGB frame onto the panel */
int quit_asked(void);
/* 'k' (a 1 down/0 up, b code), 't' (a 'd'/'m'/'u', b x, c y), 'w' (a travel), 'v' (a level) */
int next_event(int *kind, int *a, int *b, int *c);
uint64_t event_time(void); /* when the last event came, now_us */
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void message(const char *title, const char *body); /* a full-screen note, in the plain font */
const char *app_dir(void);
const char *music_root(void); /* the card: /mnt/mmc */
const char *ffmpeg_path(void);
const char *output_name(void); /* argv[1]: plughw:0,0, plug:bluealsa, usb, or "" */
int volume_can(void);          /* the firmware takes /tmp/q2-hero.volume */
void volume_ask(void);
void volume_set(int level);    /* 0-100 */

/* ---------------------------------------------------------------------------------------------
 * sound.c: the music, effects and the clock */
typedef struct {
    int16_t *data; /* mono, RATE */
    unsigned len;
} Sound;
void snd_start(const char *device, const char *volume);
void snd_shutdown(void);
int snd_on(void); /* a device is playing (else a silent clock runs at the same rate) */
void snd_gain_arg(const char *volume); /* a new soft volume ("NN" or "hNN"), from a 'v' datagram */
void snd_play(const Sound *s, int vol, int pan); /* vol 0-256, pan -256..256 */
void snd_play_at(const Sound *s, int vol, int pan, int64_t frame); /* at a music frame */
void snd_sfx_volume(int v);   /* 0-256 */
void snd_music_volume(int v); /* 0-256 */

/* The music: frames come from a ring that decode.c fills, from silence (the calibration), or
 * none. The position counts from -lead, so the song begins after the lead-in. */
enum { MUS_OFF, MUS_RING, MUS_SILENT };
void snd_music(int mode, int64_t lead_frames);
void snd_pause(int paused);
int snd_music_ended(void);        /* the ring ran dry with its decoder done */
double snd_time(void);            /* seconds of music heard now (negative in the lead-in) */
int64_t snd_written(void);        /* the music frame mixed last */
void snd_muffle(void);            /* a missed note: the music dulls for a moment */
/* the ring, for decode.c */
unsigned ring_space(void);
void ring_put(const int16_t *frames, unsigned n);
void ring_reset(void);
void ring_eof(int eof);

/* sfx.c */
enum { SFX_TICK, SFX_SELECT, SFX_BACK, SFX_MISS, SFX_OVER, SFX_HERO_READY, SFX_HERO_GO, SFX_STICK,
       SFX_STAR, SFX_FAIL, SFX_CLICK, SFX_N };
extern Sound sfx[SFX_N];
void sfx_make(void);

/* ---------------------------------------------------------------------------------------------
 * decode.c: songs through the Q2's ffmpeg */
typedef struct {
    char title[128], artist[128];
    double duration; /* 0 unknown */
} SongInfo;
/* the whole song, mono at AN_RATE, handed in blocks to sink; 0 ok, -1 failed, 1 cancelled */
int dec_listen(const char *file, SongInfo *info, void (*sink)(const int16_t *, unsigned, void *), void *ctx,
               volatile int *cancel, volatile float *progress);
int dec_stream_start(const char *file); /* into sound.c's ring, at RATE, stereo */
void dec_stream_stop(void);

/* ---------------------------------------------------------------------------------------------
 * analyze.c: listening to a song */
enum { KIND_TUNE, KIND_BASS, KIND_KICK, KIND_SNARE, KIND_HAT, KINDS };
typedef struct {
    float t;        /* s */
    float strength; /* 0..1 within the song */
    float pitch;    /* a note's pitch, MIDI (the tune and the bass) */
    float dur;      /* s the note holds */
    float local;    /* the music's loudness here, 0..1.5 */
    float wobble;   /* the tune's: how far its pitch wavers, semitones (a voice's vibrato and slides) */
    uint8_t kind;   /* KIND_*: a note of the tune or the bass, or a drum */
    uint8_t crash;  /* a crash: kick and hats at once */
} Onset;

typedef struct {
    SongInfo info;
    float duration, bpm;
    int nbeats, nonsets;
    float *beat; /* s */
    Onset *on;
    float *level; /* loudness each 0.25 s, 0..1, for the stage lights */
    int nlevel;
    uint8_t *curve; /* each hop's (128 samples at AN_RATE) rises, bass mid high kick snare hats, to the
                       song's scale x 40, then loudness x 150, then how many pitch frames (4 hops)
                       the longest partial begun there lasts: CURVES bytes a hop */
    int ncurve;
    int swing; /* the beat splits in thirds (a shuffle), not halves and quarters */
} Analysis;
#define CURVES 8
float hop_seconds(int hop); /* analyze.c: a hop's time, as notes are timed */

typedef struct Listener Listener;
Listener *listen_new(void);
void listen_feed(Listener *l, const int16_t *pcm, unsigned n); /* n stereo frames, interleaved */
float listen_meter(Listener *l, int band); /* live, for the loading screen: 0..1 */
int listen_finish(Listener *l, Analysis *a); /* frees l; 0 ok */
void analysis_free(Analysis *a);
int analysis_save(const Analysis *a, const char *path, long size);
int analysis_load(Analysis *a, const char *path, long size);

/* ---------------------------------------------------------------------------------------------
 * chart.c */
enum { EASY, MEDIUM, HARD, EXPERT, DIFFS };
extern const char *const diff_name[DIFFS];
typedef struct {
    float t, len;    /* s; len 0 for a tap */
    uint8_t lanes;   /* bit per lane: 1 Prev, 2 Centre, 4 Next */
    uint8_t hero;    /* in a hero phrase */
    uint8_t phrase;  /* the phrase's last note */
    /* play state */
    uint8_t hit, missed, held, gone;
    uint8_t pressed; /* the lanes of a chord pressed so far */
    float press_t;
    float held_to; /* s of the sustain held */
} Note;
typedef struct {
    Note *n;
    int count;
    float window; /* s either side */
    int base_score;
} Chart;
int chart_make(const Analysis *a, int diff, Chart *c);
void chart_free(Chart *c);
float chart_miss_drain(int diff);

/* ---------------------------------------------------------------------------------------------
 * gfx.c: drawing into the frame */
typedef struct {
    uint16_t cp;
    uint8_t w, h;
    int8_t bx, by; /* from the pen and the line's top */
    uint8_t adv;
    uint32_t off;
} Glyph;
typedef struct {
    int size, ascent, descent, n;
    const Glyph *g;
    const unsigned char *px;
} FontData;
extern const FontData font_small, font_med, font_big, font_huge;
const Glyph *sysfont_glyph(const FontData *f, unsigned cp, const unsigned char **px); /* sysfont.c */
#define F_SMALL (&font_small)
#define F_MED (&font_med)
#define F_BIG (&font_big)
#define F_HUGE (&font_huge)

extern uint32_t *fb; /* VIEW_W x VIEW_H */
void fill(uint32_t c);
void gradient(int y0, int y1, uint32_t top, uint32_t bottom);
void rect(int x, int y, int w, int h, uint32_t c, int alpha); /* alpha 0-256 */
void rrect(int x, int y, int w, int h, int r, uint32_t c, int alpha);
void rrect_line(int x, int y, int w, int h, int r, uint32_t c, int alpha);
void disc(float cx, float cy, float r, uint32_t c, int alpha);
void ellipse(float cx, float cy, float rx, float ry, uint32_t c, int alpha);
void ring(float cx, float cy, float rx, float ry, float thick, uint32_t c, int alpha);
void glow(float cx, float cy, float r, uint32_t c, int alpha); /* additive, soft */
void line(float x0, float y0, float x1, float y1, float w, uint32_t c, int alpha);
void quad(const float *xy, uint32_t c, int alpha); /* 4 points, convex, any order around */
void star(float cx, float cy, float r, uint32_t c, int alpha);
void darken(int alpha);
int text_w(const FontData *f, const char *s);
void text(const FontData *f, const char *s, int x, int y, uint32_t c, int alpha); /* y: the line's top */
void text_c(const FontData *f, const char *s, int cx, int y, uint32_t c, int alpha);
void text_r(const FontData *f, const char *s, int rx, int y, uint32_t c, int alpha);
void text_shadow(const FontData *f, const char *s, int x, int y, uint32_t c, int alpha);
void text_fit(const FontData *f, const char *s, int x, int y, int maxw, uint32_t c, int alpha); /* … if long */
extern const int logo_w, logo_h;
extern const unsigned char logo_px[]; /* grey, alpha */
void logo(int x, int y);
void text_grad(const FontData *f, const char *s, int cx, int y, uint32_t top, uint32_t bottom, uint32_t edge);
static inline uint32_t mix(uint32_t a, uint32_t b, int t) { /* t 0-256 toward b */
    t = t < 0 ? 0 : t > 256 ? 256 : t;
    uint32_t rb = ((a & 0xff00ff) * (uint32_t)(256 - t) + (b & 0xff00ff) * (uint32_t)t) >> 8 & 0xff00ff;
    uint32_t g = ((a & 0xff00) * (uint32_t)(256 - t) + (b & 0xff00) * (uint32_t)t) >> 8 & 0xff00;
    return rb | g;
}
void clip(int x0, int y0, int x1, int y1);
void clip_reset(void);

/* ---------------------------------------------------------------------------------------------
 * maths */
float fsin(float a);
float fcos(float a);
float flog(float x);
float fexp(float x);
static inline float fsqrt(float x) { return __builtin_sqrtf(x); }
static inline float fabs_(float x) { return x < 0 ? -x : x; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int min_i(int a, int b) { return a < b ? a : b; }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
unsigned rnd(void);
float rndf(void); /* 0..1 */
uint32_t fnv(const char *s, uint32_t h);

/* ---------------------------------------------------------------------------------------------
 * store.c: settings, best scores, the charts kept on the card */
typedef struct {
    int speed;     /* 1..5 */
    int lefty;     /* lanes mirrored */
    int nofail;
    int sfx;       /* 0 off, 1 low, 2 high */
    int offset_ms[3]; /* the jack, Bluetooth, USB */
    char last_dir[512];
    int last_diff;
} Settings;
extern Settings set;
void settings_load(void);
void settings_save(void);
int *offset_for_output(void);
typedef struct {
    int score, stars, pct, streak;
} Best;
Best best_get(uint32_t song, int diff);
int best_put(uint32_t song, int diff, Best b); /* 1 a new best */
int best_any(uint32_t song); /* the most stars on any difficulty, -1 never played */
void cache_path(char *out, int n, uint32_t song);
long file_size(const char *path);

/* ---------------------------------------------------------------------------------------------
 * browse.c, game.c, menu.c */
typedef struct {
    int wheel;                          /* travel since the last frame */
    int centre, prev, next, play, back; /* pressed since the last frame */
    int down[3];                        /* lane keys held: Prev, Centre, Next */
    int tap, tap_x, tap_y;              /* a touch that began and ended without moving far */
    int touch, touch_x, touch_y, touch_start_y; /* a finger down now */
    int drag;                           /* moved since the last frame, px */
} Input;
extern Input in;
void input_frame(void);  /* gathers events into in, for the menus */
extern int volume_level; /* the Q2's, -1 unknown */

int tags_read(const char *path, char *title, int tn, char *artist, int an); /* tags.c: 1 a title */
int game_main(void);
#endif
