/* Q2 Hero: the screens and the game.
 *
 *   Title -> Songs (the card's folders) -> Listening (once a song) -> Difficulty -> Play
 *   Play -> Pause (Return) or Results;  Title -> Options -> Calibrate
 *
 * Playing: notes come down three lanes to the fret targets; press the lane's button as each
 * crosses, Prev, Centre or Next (or tap the lane). Hold a held note's button to its end. Prev and
 * Next together are a chord. Streaks raise the multiplier to 4x; a whole Hero phrase (the star
 * notes) fills a quarter of Hero Power, and from half full Play/Pause doubles the multiplier
 * until it drains. Misses drain the rock meter, and it running out fails the song (unless No
 * Fail). The wheel is the volume while playing.
 *
 * Everything is timed by the music as heard (sound.c), less the player's timing offset. */
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "hero.h"

#define FPS_US 16667
#define IDLE_QUIT_US (5ull * 60 * 1000000) /* menus left this long: Q2 Hero closes itself */
#define STRIKE_Y 262.0f
#define VANISH_Y (-47.0f)
#define BOARD_HALF 112.0f /* px at the strike line */
#define DEPTH 3.0f        /* the board's far end, in units of the near distance */
#define LANE_W 75.0f
#define HERO_DRAIN (1.0f / 16) /* a full meter lasts 16 s */

enum { S_TITLE, S_BROWSE, S_LISTEN, S_SONG, S_PLAY, S_PAUSE, S_RESULTS, S_OPTIONS, S_CALIBRATE, S_NOTE, S_VOLUME };

static const uint32_t lane_col[LANES] = { 0x22dd55, 0xff3344, 0xffcc1a };
static const uint32_t lane_dark[LANES] = { 0x0b5a22, 0x6a1018, 0x6a5208 };
static const uint32_t ORANGE = 0xff8a1a, GOLD = 0xffd24a, WHITE = 0xffffff, GREY = 0x9a9aa8, HERO_BLUE = 0x48c8ff;

Input in;
int volume_level = -1;
static int state = S_TITLE;
static uint64_t frame_t, last_input_t;
static float anim; /* s since start, for the menus' motion */

/* ---------------------------------------------------------------------------------------------
 * Input for the menus */
static int wheel_acc;
static int keys_down[256];

static void lane_key(int code, int down);
static void touch_event(int kind, int x, int y);
static int playing_input; /* lane keys and touches go to the game */
static void calib_press(void);

void input_frame(void) {
    int kind, a, b, c;
    in.wheel = in.centre = in.prev = in.next = in.play = in.back = in.tap = 0;
    in.drag = 0;
    while (next_event(&kind, &a, &b, &c)) {
        last_input_t = now_us();
        if (kind == 'k') {
            int code = b & 255, was = keys_down[code];
            keys_down[code] = a;
            if (playing_input && (b == K_PREV || b == K_CENTRE || b == K_NEXT)) {
                if (a != was) lane_key(b, a);
                continue;
            }
            if (!a || was) continue; /* a release, or a held key's repeat */
            if (state == S_CALIBRATE && b == K_CENTRE) calib_press(); /* stamped as it came */
            if (b == K_CENTRE) in.centre = 1;
            else if (b == K_PREV) in.prev = 1;
            else if (b == K_NEXT) in.next = 1;
            else if (b == K_PLAY) in.play = 1;
            else if (b == K_RETURN) in.back = 1;
        } else if (kind == 't') {
            if (playing_input) {
                touch_event(a, b, c);
                continue;
            }
            if (a == 'd') in.touch = 1, in.touch_x = b, in.touch_y = c, in.touch_start_y = c;
            else if (a == 'm' && in.touch) in.drag += c - in.touch_y, in.touch_x = b, in.touch_y = c;
            else if (a == 'u' && in.touch) {
                in.touch = 0;
                if (abs(c - in.touch_start_y) < 12) in.tap = 1, in.tap_x = b, in.tap_y = c;
            }
        } else if (kind == 'w')
            in.wheel += a;
        else if (kind == 'v')
            volume_level = a;
    }
}

/* the wheel as list steps: clockwise (negative travel) is down */
static int wheel_steps(int per) {
    wheel_acc -= in.wheel;
    int s = wheel_acc / per;
    wheel_acc -= s * per;
    return s;
}

static void play_sfx(int id, int vol) {
    static const int level[3] = { 0, 150, 256 };
    if (set.sfx) snd_play(&sfx[id], vol * level[set.sfx] >> 8, 0);
}

/* ---------------------------------------------------------------------------------------------
 * The highway's projection: dz from the strike line (0) to the far end (DEPTH) */
static float sc_of(float z) { return 1 / (1 + z); }
static float y_of(float z) { return VANISH_Y + (STRIKE_Y - VANISH_Y) * sc_of(z); }
static float lane_x(int screen_lane, float z) { return VIEW_W / 2.0f + (screen_lane - 1) * LANE_W * sc_of(z); }
static int screen_lane(int lane) { return set.lefty ? 2 - lane : lane; }
static float visible_s(void) { return 2.3f - (set.speed - 1) * 0.33f; } /* 2.3 s .. 0.98 s */

static void board(float now, const float *beat, int nbeats, int hero_on, float pulse) {
    float zfar = DEPTH;
    /* the board: rows from the far end to the bottom of the screen */
    for (int y = (int)y_of(zfar); y < VIEW_H; ++y) {
        float s = (y - VANISH_Y) / (STRIKE_Y - VANISH_Y);
        float half = BOARD_HALF * s, x0 = VIEW_W / 2.0f - half, x1 = VIEW_W / 2.0f + half;
        float fade = clampf((s - sc_of(zfar)) / 0.08f, 0, 1);
        uint32_t col = mix(0x14121c, 0x232030, (int)(s * 160));
        int a = (int)(fade * 256);
        uint32_t *row = fb + y * VIEW_W;
        int xa = (int)x0, xb = (int)x1;
        if (xa >= 0 && xa < VIEW_W) row[xa] = mix(row[xa], col, (int)((1 - (x0 - xa)) * a));
        if (xb >= 0 && xb < VIEW_W) row[xb] = mix(row[xb], col, (int)((x1 - xb) * a));
        for (int x = clampi(xa + 1, 0, VIEW_W); x < clampi(xb, 0, VIEW_W); ++x) row[x] = a >= 256 ? col : mix(row[x], col, a);
        /* lane lines and rails */
        for (int k = 0; k <= 3; ++k) {
            float lx = VIEW_W / 2.0f + (k - 1.5f) * LANE_W * s;
            int rail = k == 0 || k == 3;
            uint32_t c = rail ? (hero_on ? HERO_BLUE : 0xb8b0c8) : 0x4a4660;
            int ra = (int)(fade * (rail ? 230 : 150));
            float w = rail ? 2.2f * s + 0.6f : 1.1f * s + 0.3f;
            int xa = (int)(lx - w * 0.5f), xb = (int)(lx + w * 0.5f);
            for (int x = xa; x <= xb; ++x)
                if (x >= 0 && x < VIEW_W) fb[y * VIEW_W + x] = mix(fb[y * VIEW_W + x], c, ra);
        }
    }
    /* beat lines, the bar's first thicker */
    float vis = visible_s();
    for (int i = 0; i < nbeats; ++i) {
        float dt = beat[i] - now;
        if (dt < -0.25f || dt > vis) continue;
        float z = dt / vis * DEPTH, s = sc_of(z), y = y_of(z), half = BOARD_HALF * s;
        int bar = i % 4 == 0;
        float fade = clampf((DEPTH - z) / 0.5f, 0, 1) * clampf((dt + 0.25f) / 0.15f, 0, 1);
        line(VIEW_W / 2.0f - half, y, VIEW_W / 2.0f + half, y, (bar ? 2.6f : 1.3f) * s + 0.4f,
             bar ? 0x9088b0 : 0x5a5478, (int)(fade * (bar ? 220 : 150)));
    }
    (void)pulse;
}

static void gem(float x, float y, float s, int lane, int hero, int alpha, float lit) {
    float rx = 27 * s, ry = 13.5f * s;
    uint32_t c = hero ? 0xdff4ff : lane_col[lane], dark = hero ? 0x2a6a9a : lane_dark[lane];
    ellipse(x, y + 3 * s, rx * 1.02f, ry, 0, alpha / 2);               /* shadow */
    ellipse(x, y, rx + 1.5f, ry + 1.5f, 0x0a0a0a, alpha);             /* rim */
    ellipse(x, y - 1 * s, rx, ry, dark, alpha);                        /* side */
    ellipse(x, y - 3.5f * s, rx * 0.92f, ry * 0.86f, c, alpha);         /* top */
    ellipse(x, y - 3.5f * s, rx * 0.55f, ry * 0.5f, mix(c, 0x101010, 110), alpha); /* the cup */
    ellipse(x - rx * 0.12f, y - 5 * s, rx * 0.32f, ry * 0.26f, WHITE, alpha * 3 / 4);
    if (hero) star(x, y - 4 * s, rx * 0.62f, mix(lane_col[lane], WHITE, 90), alpha);
    if (lit > 0) glow(x, y - 3 * s, rx * 1.6f, c, (int)(lit * 160));
}

/* ---------------------------------------------------------------------------------------------
 * Particles: sparks and flames at the fret targets */
static struct {
    float x, y, vx, vy, life, max;
    uint32_t c;
    int kind; /* 0 spark, 1 flame */
} parts[200];

static void spawn(float x, float y, uint32_t c, int n, int flame) {
    for (int i = 0; i < 200 && n > 0; ++i) {
        if (parts[i].life > 0) continue;
        float a = -PI / 2 + (rndf() - 0.5f) * (flame ? 1.1f : 2.6f), v = flame ? 60 + rndf() * 90 : 90 + rndf() * 180;
        parts[i].x = x + (rndf() - 0.5f) * 20, parts[i].y = y;
        parts[i].vx = fcos(a) * v, parts[i].vy = fsin(a) * v;
        parts[i].life = parts[i].max = flame ? 0.35f + rndf() * 0.25f : 0.25f + rndf() * 0.35f;
        parts[i].c = c, parts[i].kind = flame;
        --n;
    }
}
static void particles(float dt) {
    for (int i = 0; i < 200; ++i) {
        if (parts[i].life <= 0) continue;
        parts[i].life -= dt;
        parts[i].x += parts[i].vx * dt, parts[i].y += parts[i].vy * dt;
        parts[i].vy += (parts[i].kind ? -60 : 420) * dt;
        float k = parts[i].life / parts[i].max;
        if (parts[i].kind) glow(parts[i].x, parts[i].y, 6 + (1 - k) * 12, mix(0xff3a08, parts[i].c, (int)(k * 256)), (int)(k * 220));
        else disc(parts[i].x, parts[i].y, 1.4f, mix(WHITE, parts[i].c, 120), (int)(k * 256));
    }
}

/* ---------------------------------------------------------------------------------------------
 * Background: the stage */
static void stage(float level, float beat_pulse, int hero) {
    uint32_t top = hero ? 0x081a3a : 0x1a0a2e, bottom = 0x050308;
    gradient(0, VIEW_H, top, bottom);
    float l = clampf(level, 0, 1);
    for (int k = 0; k < 3; ++k) { /* the lights over the stage, swaying */
        float x = 62 + k * 125 + fsin(anim * (0.6f + k * 0.17f) + k) * 28;
        uint32_t c = hero ? HERO_BLUE : (k & 1 ? 0xff3a7a : 0xff9a2a);
        glow(x, -14, 72 + l * 26, c, (int)(44 + l * 56 + beat_pulse * 44));
    }
}

static void stars_row(int cx, int y, int n, int max, float r, int alpha) {
    for (int i = 0; i < max; ++i) {
        float x = cx + (i - (max - 1) * 0.5f) * r * 2.3f;
        star(x, (float)y, r, i < n ? GOLD : 0x3a3648, alpha);
    }
}

/* ---------------------------------------------------------------------------------------------
 * The songs on the card */
typedef struct {
    char name[256];
    int dir, stars;
    char title[200], artist[120]; /* from the song's tags, once tagged */
    volatile int tagged;
} Entry;
static Entry *ents;
static int nents, sel, top_row;
static float scroll_y;

static const char *const audio_ext[] = { "mp3", "flac", "m4a", "aac", "ogg", "oga", "opus", "wav", "aif", "aiff",
                                        "ape", "wv", "wma", "alac", "mka", "mp2", "dsf", "dff", "m4b", "webm", 0 };
static int is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (int i = 0; audio_ext[i]; ++i)
        if (!strcasecmp(dot + 1, audio_ext[i])) return 1;
    return 0;
}
static int by_name(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}
static void join(char *out, int n, const char *dir, const char *name) { snprintf(out, (size_t)n, "%s/%s", dir, name); }

/* ---------------------------------------------------------------------------------------------
 * Titles from the tags (tags.c), read by a thread while the list is shown, and kept in
 * tags.txt ("hash<TAB>size<TAB>title<TAB>artist" a line) so a folder seen before has them at once */
typedef struct {
    uint32_t hash;
    long size;
    char *title, *artist;
} TagRow;
static TagRow *tag_rows;
static int ntag_rows, tag_cap, tags_loaded, tags_dirty;
static struct {
    pthread_t thread;
    int on;
    volatile int stop;
} tagger;

static void tag_remember(uint32_t hash, long size, const char *title, const char *artist) {
    if (ntag_rows == tag_cap) {
        TagRow *m = realloc(tag_rows, (size_t)(tag_cap = tag_cap ? tag_cap * 2 : 256) * sizeof *m);
        if (!m) return;
        tag_rows = m;
    }
    TagRow *r = &tag_rows[ntag_rows];
    r->hash = hash, r->size = size, r->title = strdup(title), r->artist = strdup(artist);
    if (r->title && r->artist) ++ntag_rows;
    else free(r->title), free(r->artist);
}

static void tags_load(void) {
    if (tags_loaded) return;
    tags_loaded = 1;
    FILE *f = fopen("tags.txt", "r");
    char line[700];
    while (f && fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'), *t1, *t2, *t3;
        if (nl) *nl = 0;
        if (!(t1 = strchr(line, '\t')) || !(t2 = strchr(t1 + 1, '\t')) || !(t3 = strchr(t2 + 1, '\t'))) continue;
        *t1 = *t2 = *t3 = 0;
        tag_remember((uint32_t)strtoul(line, 0, 16), atol(t1 + 1), t2 + 1, t3 + 1);
    }
    if (f) fclose(f);
}

static void tags_save(void) {
    if (!tags_dirty) return;
    FILE *f = fopen("tags.txt.new", "w");
    if (!f) return;
    for (int i = 0; i < ntag_rows; ++i)
        fprintf(f, "%08x\t%ld\t%s\t%s\n", (unsigned)tag_rows[i].hash, tag_rows[i].size, tag_rows[i].title, tag_rows[i].artist);
    if (!fclose(f)) rename("tags.txt.new", "tags.txt"), tags_dirty = 0;
}

static void join(char *out, int n, const char *dir, const char *name);
static char cwd[512];

static void *tag_worker(void *unused) {
    (void)unused;
    tags_load();
    for (int i = 0; i < nents && !tagger.stop; ++i) {
        Entry *e = &ents[i];
        if (e->dir || e->tagged) continue;
        char p[800];
        join(p, sizeof p, cwd, e->name);
        uint32_t hash = fnv(p, 0);
        long size = file_size(p);
        int known = 0;
        for (int k = 0; k < ntag_rows && !known; ++k)
            if (tag_rows[k].hash == hash && tag_rows[k].size == size) {
                snprintf(e->title, sizeof e->title, "%s", tag_rows[k].title);
                snprintf(e->artist, sizeof e->artist, "%s", tag_rows[k].artist);
                known = 1;
            }
        if (!known) {
            tags_read(p, e->title, sizeof e->title, e->artist, sizeof e->artist);
            tag_remember(hash, size, e->title, e->artist);
            tags_dirty = 1;
        }
        __sync_synchronize();
        e->tagged = 1;
    }
    tags_save();
    return 0;
}

static void tagger_stop(void) {
    if (!tagger.on) return;
    tagger.stop = 1;
    pthread_join(tagger.thread, 0);
    tagger.on = 0;
}

static void tagger_start(void) {
    tagger_stop();
    tagger.stop = 0;
    tagger.on = !pthread_create(&tagger.thread, 0, tag_worker, 0);
}

static void read_dir(void) {
    tagger_stop();
    free(ents);
    ents = 0, nents = 0, sel = 0, top_row = 0, scroll_y = 0;
    DIR *d = opendir(cwd);
    if (!d) return;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "System Volume Information") || !strcmp(e->d_name, "LOST.DIR") ||
            !strcmp(e->d_name, "$RECYCLE.BIN"))
            continue;
        int isdir = e->d_type == 4;
        if (e->d_type == 0) { /* the card's filesystem didn't say */
            char p[800];
            join(p, sizeof p, cwd, e->d_name);
            DIR *t = opendir(p);
            if (t) isdir = 1, closedir(t);
        }
        if (!isdir && !is_audio(e->d_name)) continue;
        if (nents == cap) {
            Entry *m = realloc(ents, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *ents);
            if (!m) break;
            ents = m;
        }
        snprintf(ents[nents].name, sizeof ents[nents].name, "%s", e->d_name);
        ents[nents].dir = isdir, ents[nents].stars = -1;
        ents[nents].title[0] = ents[nents].artist[0] = 0, ents[nents].tagged = 0;
        if (!isdir) {
            char p[800];
            join(p, sizeof p, cwd, e->d_name);
            ents[nents].stars = best_any(fnv(p, 0));
        }
        ++nents;
    }
    closedir(d);
    qsort(ents, (size_t)nents, sizeof *ents, by_name);
    tagger_start();
}

static void open_dir(const char *path, const char *select) {
    char p[sizeof cwd], s[256] = "";
    tagger_stop(); /* it reads cwd */
    snprintf(p, sizeof p, "%s", path); /* path and select may be cwd and an entry's name */
    if (select) snprintf(s, sizeof s, "%s", select), select = s;
    snprintf(cwd, sizeof cwd, "%s", p);
    read_dir();
    for (int i = 0; select && i < nents; ++i)
        if (!strcmp(ents[i].name, select)) sel = i;
    top_row = clampi(sel - 3, 0, nents > 8 ? nents - 8 : 0);
    scroll_y = (float)top_row;
}

static void icon_folder(float x, float y, uint32_t c) {
    rrect((int)x, (int)y + 3, 20, 14, 2, c, 256);
    rrect((int)x, (int)y, 9, 6, 2, c, 256);
}
static void icon_note(float x, float y, uint32_t c) {
    ellipse(x + 5, y + 14, 4.5f, 3.4f, c, 256);
    line(x + 9, y + 14, x + 9, y + 1, 1.8f, c, 256);
    line(x + 9, y + 1.5f, x + 16, y + 5, 2.4f, c, 256);
}

/* ---------------------------------------------------------------------------------------------
 * The song chosen */
static char song_path[800], song_name[256];
static uint32_t song_id;
static Analysis an;
static int have_an;
static Chart chart;
static int diff = MEDIUM, diff_notes[DIFFS];

static struct {
    pthread_t thread;
    volatile int done, cancel, result;
    volatile float progress;
    Listener *l;
    int running;
} job;

static void sink(const int16_t *pcm, unsigned n, void *ctx) {
    static int slow = -1; /* the host tests' way to see the screen as a slow Q2 shows it */
    if (slow < 0) slow = getenv("HERO_SLOW_LISTEN") != 0;
    if (slow) usleep(30000);
    listen_feed((Listener *)ctx, pcm, n);
}

static void *listen_job(void *unused) {
    (void)unused;
    Analysis a;
    memset(&a, 0, sizeof a);
    SongInfo info;
    int r = dec_listen(song_path, &info, sink, job.l, &job.cancel, &job.progress);
    Listener *l = job.l;
    if (!r) {
        r = listen_finish(l, &a) ? -1 : 0;
        job.l = 0;
        if (r) analysis_free(&a);
        if (!r) {
            a.info = info;
            char cp[64];
            cache_path(cp, sizeof cp, song_id);
            analysis_save(&a, cp, file_size(song_path));
            an = a;
        }
    }
    job.result = r;
    __sync_synchronize();
    job.done = 1;
    return 0;
}

/* a listen cancelled or failed before its end: what it heard is let go */
static void drop_listener(void) {
    if (!job.l) return;
    Analysis none;
    memset(&none, 0, sizeof none);
    listen_finish(job.l, &none); /* leaves none zeroed, or filled */
    analysis_free(&none);
    job.l = 0;
}

static void job_end(void) {
    if (!job.running) return;
    job.cancel = 1;
    pthread_join(job.thread, 0);
    job.running = 0;
    drop_listener();
}

static char note_title[64], note_body[400];
static int note_back;
static void show_note(const char *title, const char *body, int back) {
    snprintf(note_title, sizeof note_title, "%s", title);
    snprintf(note_body, sizeof note_body, "%s", body);
    note_back = back;
    state = S_NOTE;
}

static void choose_song(const char *path, const char *name) {
    snprintf(song_path, sizeof song_path, "%s", path);
    snprintf(song_name, sizeof song_name, "%s", name);
    char *dot = strrchr(song_name, '.');
    if (dot && dot != song_name && strcmp(name, strrchr(path, '/') ? strrchr(path, '/') + 1 : path) == 0) *dot = 0;
    song_id = fnv(path, 0);
    if (have_an) analysis_free(&an), have_an = 0;
    char cp[64];
    cache_path(cp, sizeof cp, song_id);
    long size = file_size(path);
    if (analysis_load(&an, cp, size)) {
        have_an = 1;
        logf_("song: %s (kept: %.1f BPM, %d hits)", path, an.bpm, an.nonsets);
        state = S_SONG;
    } else {
        memset(&job, 0, sizeof job);
        job.l = listen_new();
        if (!job.l || pthread_create(&job.thread, 0, listen_job, 0)) {
            show_note("Out of memory", "There isn't enough memory free to listen to this song.", S_BROWSE);
            return;
        }
        job.running = 1;
        state = S_LISTEN;
    }
    diff = set.last_diff;
    for (int d = 0; d < DIFFS && have_an; ++d) {
        Chart c;
        diff_notes[d] = chart_make(&an, d, &c) ? c.count : 0;
        chart_free(&c);
    }
}

/* ---------------------------------------------------------------------------------------------
 * Playing */
static struct {
    float lead;      /* s before the song */
    float now;       /* the game's time: the music heard less the offset */
    uint64_t now_us; /* when now was taken */
    int score, streak, best_streak, mult, hits, misses, notes_total, phrases, phrases_hit;
    float score_f;   /* held notes' points as they come */
    float rock, hero, hero_left;
    int hero_on, hero_ready_said;
    int failed, done;
    float fail_t, end_t, fade;
    int next_note;   /* the first note not yet past */
    int held[LANES]; /* the note held in each lane, -1 */
    int touch_lane;
    float lit[LANES], press[LANES]; /* fret target flashes */
    float banner_t;
    char banner[48];
    uint32_t banner_c;
    float last_level;
    float pause_count; /* s left of the count back in */
    int pause_sel;
    int vol_acc;
    int new_best, stars;
} g;

static double offset_s(void) { return *offset_for_output() / 1000.0; }
static float game_time(void) { return (float)(snd_time() - offset_s()); }

static void banner(const char *text, uint32_t c) {
    snprintf(g.banner, sizeof g.banner, "%s", text);
    g.banner_c = c, g.banner_t = 1.4f;
}

static void hero_check_ready(void) {
    if (g.hero >= 0.5f && !g.hero_on && !g.hero_ready_said) {
        g.hero_ready_said = 1;
        play_sfx(SFX_HERO_READY, 200);
        banner("HERO POWER READY", HERO_BLUE);
    }
}

static void start_song(void) {
    chart_free(&chart);
    if (!chart_make(&an, diff, &chart) || !chart.count) {
        show_note("No notes", "Q2 Hero couldn't find a beat to chart in this song.", S_SONG);
        return;
    }
    memset(&g, 0, sizeof g);
    for (int i = 0; i < LANES; ++i) g.held[i] = -1;
    g.touch_lane = -1;
    g.rock = 0.5f, g.mult = 1;
    g.lead = visible_s() + 1.7f;
    for (int i = 0; i < chart.count; ++i) {
        g.notes_total += chart.n[i].lanes == 5 ? 2 : 1;
        if (chart.n[i].phrase) ++g.phrases;
    }
    snd_music_volume(256);
    if (!dec_stream_start(song_path)) {
        show_note("Can't play", "ffmpeg wouldn't start for this song.", S_SONG);
        return;
    }
    snd_music(MUS_RING, (int64_t)(g.lead * RATE));
    /* the drummer counts in: four sticks on the beats before the song */
    float p = an.nbeats > 1 ? (an.beat[an.nbeats - 1] - an.beat[0]) / (an.nbeats - 1) : 0.5f, first = an.beat[0];
    while (first - p > 0.05f) first -= p;
    int k = 0;
    for (float t = first - p; k < 4 && t > -g.lead + 0.3f; t -= p, ++k)
        if (set.sfx) snd_play_at(&sfx[SFX_STICK], 220, 0, (int64_t)((t + offset_s() * 0) * RATE));
    playing_input = 1;
    state = S_PLAY;
    logf_("play: %s on %s, %d notes, lead %.2f s, offset %d ms", song_name, diff_name[diff], chart.count, g.lead,
          *offset_for_output());
}

static void stop_song(void) {
    dec_stream_stop();
    snd_music(MUS_OFF, 0);
    playing_input = 0;
    memset(keys_down, 0, sizeof keys_down);
}

static int mult_now(void) { return (1 + clampi(g.streak / 10, 0, 3)) * (g.hero_on ? 2 : 1); }

/* a hero note missed or let go: the rest of its phrase are plain notes */
static void break_phrase(Note *n) {
    for (Note *e = chart.n + chart.count; n < e && n->hero; ++n) {
        n->hero = 0;
        if (n->phrase) break;
    }
}

static void note_missed(Note *n) {
    n->missed = 1;
    if (g.streak >= 10) banner("STREAK LOST", 0xff6050);
    g.streak = 0;
    g.misses += n->lanes == 5 ? 2 : 1;
    g.rock -= chart_miss_drain(diff);
    snd_muffle();
    play_sfx(SFX_MISS, 220);
    if (n->hero) break_phrase(n);
}

static void phrase_end(Note *n) {
    if (!n->phrase || !n->hero) return;
    g.hero = minf(1, g.hero + 0.25f);
    ++g.phrases_hit;
    banner("HERO PHRASE!", HERO_BLUE);
    play_sfx(SFX_STAR, 160);
    hero_check_ready();
}

static void note_hit(Note *n, int idx) {
    n->hit = 1;
    int count = n->lanes == 5 ? 2 : 1;
    g.streak += 1, g.hits += count;
    if (g.streak > g.best_streak) g.best_streak = g.streak;
    int m = mult_now();
    if (m != g.mult && !g.hero_on && m > g.mult) {
        char b[16];
        snprintf(b, sizeof b, "%dX MULTIPLIER", m);
        banner(b, GOLD);
    }
    g.mult = m;
    g.score += 50 * count * m;
    g.rock = minf(1, g.rock + (g.rock < 0.5f ? 0.035f : 0.018f));
    if (g.streak && g.streak % 50 == 0) {
        char b[32];
        snprintf(b, sizeof b, "%d NOTE STREAK!", g.streak);
        banner(b, ORANGE);
    }
    for (int l = 0; l < LANES; ++l)
        if (n->lanes >> l & 1) {
            int sl = screen_lane(l);
            g.lit[l] = 1;
            spawn(lane_x(sl, 0), STRIKE_Y - 4, n->hero ? HERO_BLUE : lane_col[l], 10, 0);
            spawn(lane_x(sl, 0), STRIKE_Y - 4, lane_col[l], 5, 1);
            if (n->len > 0) g.held[l] = idx;
        }
    if (n->len > 0) n->held = 1, n->held_to = n->t;
    else phrase_end(n);
}

/* a lane's button went down at t, game time; by touch, one finger plays a chord */
static void judge(int lane, float t, int touch) {
    int best = -1;
    float bd = 1e9f;
    for (int i = g.next_note; i < chart.count; ++i) {
        Note *n = &chart.n[i];
        if (n->t - t > chart.window) break;
        if (n->hit || n->missed || !(n->lanes >> lane & 1)) continue;
        if (n->lanes == 5 && n->pressed >> lane & 1) continue;
        float d = fabs_(n->t - t);
        if (d <= chart.window && d < bd) bd = d, best = i;
        if (d <= chart.window) break; /* the earliest one in reach */
    }
    if (best < 0) { /* nothing to play: an overstrum */
        if (g.streak >= 10) banner("STREAK LOST", 0xff6050);
        g.streak = 0, g.mult = mult_now();
        g.rock = maxf(0, g.rock - 0.015f);
        play_sfx(SFX_OVER, 170);
        return;
    }
    Note *n = &chart.n[best];
    if (n->lanes == 5 && !touch) { /* a chord: both within 120 ms */
        if (n->pressed && fabs_(t - n->press_t) > 0.12f) n->pressed = 0;
        n->pressed |= (uint8_t)(1 << lane);
        n->press_t = t;
        if (n->pressed == 5) note_hit(n, best);
        return;
    }
    note_hit(n, best);
}

static void release(int lane) {
    int i = g.held[lane];
    if (i < 0) return;
    g.held[lane] = -1;
    Note *n = &chart.n[i];
    if (!n->held) return;
    n->held = 0;
    if (n->held_to < n->t + n->len - 0.12f) { /* let go early: the rest is lost */
        n->gone = 1;
        if (n->hero) break_phrase(n);
    }
    phrase_end(n);
}

static float press_time(void) {
    uint64_t at = event_time(), now = now_us();
    return game_time() - (float)(now > at ? now - at : 0) / 1e6f;
}

static int key_lane(int code) { return code == K_PREV ? 0 : code == K_CENTRE ? 1 : 2; }
static int lane_code(int lane) { return lane == 0 ? K_PREV : lane == 1 ? K_CENTRE : K_NEXT; }

static void lane_key(int code, int down) {
    if (state != S_PLAY) return;
    int lane = key_lane(code);
    if (down) {
        g.press[lane] = 1;
        judge(lane, press_time(), 0);
    } else {
        g.press[lane] = 0;
        release(lane);
    }
}

/* a touch on the lower half of the board is a lane's button */
static int touch_lane_at(int x, int y) {
    if (y < 150) return -1;
    int sl = x < VIEW_W / 2 - LANE_W / 2 ? 0 : x < VIEW_W / 2 + LANE_W / 2 ? 1 : 2;
    return screen_lane(sl); /* screen_lane is its own inverse */
}
static void touch_event(int kind, int x, int y) {
    if (state != S_PLAY) {
        if (kind == 'u') in.tap = 1, in.tap_x = x, in.tap_y = y;
        return;
    }
    int lane = kind == 'u' ? -1 : touch_lane_at(x, y);
    if (lane == g.touch_lane) return;
    if (g.touch_lane >= 0 && !keys_down[lane_code(g.touch_lane)]) g.press[g.touch_lane] = 0, release(g.touch_lane);
    g.touch_lane = lane;
    if (lane >= 0) g.press[lane] = 1, judge(lane, press_time(), 1);
}

/* ---------------------------------------------------------------------------------------------
 * The play screen */
static void draw_play(float dt) {
    float now = g.now, vis = visible_s();
    float level = 0.5f;
    if (an.nlevel && now >= 0) level = an.level[clampi((int)(now * 4), 0, an.nlevel - 1)];
    g.last_level += (level - g.last_level) * minf(1, dt * 8);
    /* the beat's pulse */
    float pulse = 0;
    for (int i = 0; i < an.nbeats; ++i) {
        float d = now - an.beat[i];
        if (d >= 0 && d < 0.25f) pulse = 1 - d / 0.25f;
        if (an.beat[i] > now) break;
    }
    stage(g.last_level, pulse, g.hero_on);
    board(now, an.beat, an.nbeats, g.hero_on, pulse);

    /* the fret targets */
    for (int l = 0; l < LANES; ++l) {
        int sl = screen_lane(l);
        float x = lane_x(sl, 0), y = STRIKE_Y + 2;
        int down = g.press[l] > 0;
        ellipse(x, y + 4, 31, 15.5f, 0, 160);
        ellipse(x, y, 30, 15, down ? lane_col[l] : 0x15131c, 256);
        ring(x, y, 30, 15, 4.5f, lane_col[l], 256);
        ring(x, y, 22, 11, 1.5f, mix(lane_col[l], 0, 120), 200);
        if (down) ellipse(x, y - 1, 16, 8, mix(lane_col[l], WHITE, 140), 230);
        if (g.lit[l] > 0) glow(x, y - 6, 46, lane_col[l], (int)(g.lit[l] * 200)), g.lit[l] -= dt * 4;
        /* what the button is: |<<  o  >>| */
        float ly = 302;
        uint32_t ic = 0xc8c4d8;
        if (l == 1) ring(x, ly, 5.5f, 5.5f, 2, ic, 220);
        else {
            float d = l == 0 ? -1 : 1;
            float t1[6] = { x + d * 7, ly, x - d * 1, ly - 5.5f, x - d * 1, ly + 5.5f };
            float t2[6] = { x + d * 1, ly, x - d * 7, ly - 5.5f, x - d * 7, ly + 5.5f };
            float q1[8] = { t1[0], t1[1], t1[2], t1[3], t1[4], t1[5], t1[0], t1[1] };
            float q2[8] = { t2[0], t2[1], t2[2], t2[3], t2[4], t2[5], t2[0], t2[1] };
            quad(q1, ic, 220), quad(q2, ic, 220);
            rect((int)(x + d * 8.5f) - (d > 0 ? 0 : 2), (int)ly - 5, 2, 11, ic, 220);
        }
    }
    /* held tails, then the gems, far to near */
    for (int i = chart.count - 1; i >= g.next_note; --i) {
        Note *n = &chart.n[i];
        if (n->t - now > vis) continue;
        if (n->len <= 0 || n->missed || (n->hit && !n->held && !n->gone && n->held_to >= n->t + n->len - 0.13f)) continue;
        float t0 = n->held ? now : n->t, t1 = n->t + n->len;
        if (t1 < now - 0.2f) continue;
        if (n->gone || (n->hit && !n->held)) t0 = maxf(t0, now);
        for (int l = 0; l < LANES; ++l) {
            if (!(n->lanes >> l & 1)) continue;
            int sl = screen_lane(l);
            int segs = 10;
            for (int s = 0; s < segs; ++s) {
                float ta = t0 + (t1 - t0) * s / segs, tb = t0 + (t1 - t0) * (s + 1) / segs;
                float za = clampf((ta - now) / vis * DEPTH, -0.1f, DEPTH), zb = clampf((tb - now) / vis * DEPTH, -0.1f, DEPTH);
                if (za >= DEPTH) break;
                float wob = n->held ? fsin(anim * 40 + s * 1.3f) * 2.5f : 0;
                float xa = lane_x(sl, za) + wob * sc_of(za), xb = lane_x(sl, zb) + wob * sc_of(zb);
                float wa = 6 * sc_of(za), wb = 6 * sc_of(zb), ya = y_of(za), yb = y_of(zb);
                float q[8] = { xa - wa, ya, xa + wa, ya, xb + wb, yb, xb - wb, yb };
                uint32_t c = n->gone ? 0x505060 : n->held ? mix(lane_col[l], WHITE, 90) : lane_col[l];
                quad(q, n->hero && !n->gone ? mix(c, HERO_BLUE, 120) : c, n->gone ? 150 : 230);
            }
        }
    }
    for (int i = chart.count - 1; i >= g.next_note; --i) {
        Note *n = &chart.n[i];
        float dt_n = n->t - now;
        if (dt_n > vis || n->hit) continue;
        float z = dt_n / vis * DEPTH;
        if (z < -0.35f) continue;
        float s = sc_of(z), fade = clampf((DEPTH - z) / 0.35f, 0, 1);
        int alpha = (int)(fade * (n->missed ? 110 : 256));
        if (n->lanes == 5) {
            float y = y_of(z);
            line(lane_x(screen_lane(0), z), y - 3 * s, lane_x(screen_lane(2), z), y - 3 * s, 4 * s, 0xd0d0e0, alpha * 3 / 4);
        }
        for (int l = 0; l < LANES; ++l)
            if (n->lanes >> l & 1) {
                if (n->lanes == 5 && n->pressed >> l & 1) continue;
                gem(lane_x(screen_lane(l), z), y_of(z), s, l, n->hero, alpha, 0);
            }
    }
    particles(dt);

    /* the HUD */
    char b[48];
    snprintf(b, sizeof b, "%d", g.score);
    text_shadow(F_BIG, b, 26, 20, WHITE, 256);
    /* the multiplier */
    int m = mult_now();
    float mx = 336, my = 52;
    disc(mx, my, 24, 0x101018, 230);
    float prog = m >= 4 && !g.hero_on ? 1 : (g.streak % 10) / 10.0f;
    if (g.streak >= 30) prog = 1;
    for (int k = 0; k < 10; ++k) { /* ten pips round it */
        float a = -PI / 2 + k * TAU / 10;
        disc(mx + fcos(a) * 20, my + fsin(a) * 20, 2.6f, k < (int)(prog * 10 + 0.01f) ? GOLD : 0x3a3648, 256);
    }
    snprintf(b, sizeof b, "%dx", m);
    text_c(F_BIG, b, (int)mx, (int)my - 17, g.hero_on ? HERO_BLUE : m > 1 ? GOLD : WHITE, 256);
    if (g.streak >= 5) {
        snprintf(b, sizeof b, "%d", g.streak);
        text_c(F_SMALL, b, (int)mx, (int)my + 26, GREY, 256);
    }
    /* the rock meter: left */
    {
        int x = 16, y0 = 104, h = 128;
        rrect(x, y0, 14, h, 6, 0x101018, 220);
        int fillh = (int)(g.rock * (h - 4));
        uint32_t c = g.rock < 0.25f ? 0xff3030 : g.rock < 0.5f ? 0xffb020 : 0x30e060;
        rrect(x + 2, y0 + 2 + (h - 4 - fillh), 10, fillh, 4, c, 240);
        if (g.rock < 0.25f && (int)(anim * 6) % 2) rrect_line(x, y0, 14, h, 6, 0xff3030, 220);
        text_c(F_SMALL, "ROCK", x + 7, y0 + h + 4, GREY, 200);
    }
    /* Hero Power: right */
    {
        int x = 345, y0 = 104, h = 128;
        rrect(x, y0, 14, h, 6, 0x101018, 220);
        int fillh = (int)(g.hero * (h - 4));
        rrect(x + 2, y0 + 2 + (h - 4 - fillh), 10, fillh, 4, g.hero >= 0.5f ? HERO_BLUE : 0x2a78b0, 240);
        line((float)x, y0 + h / 2.0f, (float)x + 14, y0 + h / 2.0f, 1, 0x80808a, 200);
        if (g.hero_on) glow(x + 7.0f, y0 + h - fillh / 2.0f, 24, HERO_BLUE, 120);
        text_c(F_SMALL, "HERO", x + 7, y0 + h + 4, g.hero >= 0.5f ? HERO_BLUE : GREY, 200);
        if (g.hero >= 0.5f && !g.hero_on && (int)(anim * 3) % 2) text_c(F_SMALL, "Play", x + 7, y0 - 20, HERO_BLUE, 230);
    }
    /* the song so far */
    float len = an.duration > 1 ? an.duration : 1;
    rect(64, 314, 247, 3, 0x2a2638, 220);
    rect(64, 314, (int)(247 * clampf(now / len, 0, 1)), 3, ORANGE, 230);
    /* the count in */
    if (now < 0) {
        int sec = (int)(-now) + 1;
        if (-now < 3.0f && chart.n[0].t > 0.0f) {
            snprintf(b, sizeof b, "%d", sec);
            text_c(F_HUGE, b, VIEW_W / 2, 80, mix(ORANGE, WHITE, 100), clampi((int)((-now - (sec - 1)) * 400), 0, 256));
        } else
            text_c(F_BIG, "GET READY", VIEW_W / 2, 100, WHITE, 230);
    }
    if (g.banner_t > 0) {
        int a = clampi((int)(g.banner_t * 400), 0, 256);
        text_c(F_BIG, g.banner, VIEW_W / 2 + 1, 82, 0, a * 2 / 3);
        text_c(F_BIG, g.banner, VIEW_W / 2, 80, g.banner_c, a);
        g.banner_t -= dt;
    }
}

static uint64_t draw_us, draw_max, draw_n; /* the cost of drawing a play frame */
static void finish_song(void) {
    stop_song();
    g.done = 1;
    g.score += (int)g.score_f, g.score_f = 0;
    float r = chart.base_score ? (float)g.score / chart.base_score : 0;
    g.stars = g.failed ? 0 : r >= 2.6f ? 5 : r >= 1.9f ? 4 : r >= 1.25f ? 3 : r >= 0.7f ? 2 : 1;
    g.new_best = 0;
    if (!g.failed) {
        Best b = { g.score, g.stars, g.notes_total ? g.hits * 100 / g.notes_total : 0, g.best_streak };
        g.new_best = best_put(song_id, diff, b);
    }
    g.end_t = 0;
    state = S_RESULTS;
    if (draw_n) logf_("drawing: %.2f ms a frame on average, %.2f at most, %llu frames", draw_us / 1000.0 / draw_n,
                      draw_max / 1000.0, (unsigned long long)draw_n);
    draw_us = draw_max = draw_n = 0;
    logf_("result: %s %s: score %d, %d/%d notes, streak %d, %d stars%s%s", song_name, diff_name[diff], g.score, g.hits,
          g.notes_total, g.best_streak, g.stars, g.failed ? ", failed" : "", g.new_best ? ", a new best" : "");
}

static void update_play(float dt) {
    g.now = game_time();
    g.now_us = now_us();
    float now = g.now;
    for (int i = g.next_note; i < chart.count; ++i) {
        Note *n = &chart.n[i];
        if (n->t - now > chart.window) break;
        if (!n->hit && !n->missed && now - n->t > chart.window) note_missed(n);
    }
    for (int l = 0; l < LANES; ++l) {
        int i = g.held[l];
        if (i < 0) continue;
        Note *n = &chart.n[i];
        float to = minf(now, n->t + n->len);
        if (to > n->held_to) {
            g.score_f += (to - n->held_to) * 25 * mult_now();
            if (n->hero && g.hero_on) g.hero = minf(1, g.hero + (to - n->held_to) * 0.02f);
            n->held_to = to;
        }
        if ((int)(anim * 30) % 3 == 0) spawn(lane_x(screen_lane(l), 0), STRIKE_Y - 4, lane_col[l], 1, 1);
        g.lit[l] = maxf(g.lit[l], 0.5f);
        if (now >= n->t + n->len) { /* held to the end */
            g.held[l] = -1;
            n->held = 0;
            phrase_end(n);
        }
    }
    if (g.score_f >= 1) g.score += (int)g.score_f, g.score_f -= (int)g.score_f;
    while (g.next_note < chart.count) {
        Note *n = &chart.n[g.next_note];
        if ((n->hit || n->missed) && !n->held && n->t + n->len < now - 0.6f) ++g.next_note;
        else break;
    }
    if (g.hero_on) {
        g.hero -= HERO_DRAIN * dt;
        if (g.hero <= 0) g.hero = 0, g.hero_on = 0, g.hero_ready_said = 0, g.mult = mult_now();
    }
    if (!set.nofail && g.rock <= 0 && !g.failed) {
        g.failed = 1, g.fail_t = 0;
        snd_pause(1);
        play_sfx(SFX_FAIL, 256);
        banner("SONG FAILED", 0xff4040);
    }
    if (g.failed) {
        g.fail_t += dt;
        if (g.fail_t > 2.2f) finish_song();
        return;
    }
    Note *last = &chart.n[chart.count - 1];
    float end = last->t + last->len;
    if (snd_music_ended() && now > end + 0.3f) finish_song(); /* the clock stops with the music */
    else if (now > end + 1.0f) {
        if (now > an.duration + 0.5f) finish_song();
        else if (now > end + 5) { /* a long ending: fade it */
            g.fade += dt / 2.5f;
            snd_music_volume((int)(256 * (1 - clampf(g.fade, 0, 1))));
            if (g.fade >= 1) finish_song();
        }
    }
}

static void play_frame(float dt) {
    if (in.play && g.hero >= 0.5f && !g.hero_on && !g.failed) {
        g.hero_on = 1, g.mult = mult_now();
        play_sfx(SFX_HERO_GO, 256);
        banner("HERO POWER!", HERO_BLUE);
    }
    if (in.back && !g.failed) {
        snd_pause(1);
        playing_input = 0;
        for (int l = 0; l < LANES; ++l) g.press[l] = 0, release(l);
        g.pause_sel = 0, g.pause_count = 0;
        state = S_PAUSE;
        play_sfx(SFX_BACK, 200);
        return;
    }
    update_play(dt);
    if (state == S_PLAY) {
        uint64_t t0 = now_us();
        draw_play(dt);
        uint64_t d = now_us() - t0;
        draw_us += d, ++draw_n;
        if (d > draw_max) draw_max = d;
    }
}

static void menu_rows(const char *const *rows, int n, int sel_row, int y0, int h, int w) {
    for (int i = 0; i < n; ++i) {
        int y = y0 + i * h, x = VIEW_W / 2 - w / 2;
        if (i == sel_row) {
            rrect(x, y, w, h - 6, (h - 6) / 2, ORANGE, 240);
            rrect(x, y, w, (h - 6) / 2, (h - 6) / 2, 0xffb050, 120);
        } else
            rrect(x, y, w, h - 6, (h - 6) / 2, 0x000000, 120);
        text_c(F_MED, rows[i], VIEW_W / 2, y + (h - 6) / 2 - 11, i == sel_row ? 0x1a0a00 : WHITE, 256);
    }
}

/* a tap on one of menu_rows' rows: its index, or -1 */
static int row_tapped(int n, int y0, int h, int w) {
    if (!in.tap) return -1;
    int x = VIEW_W / 2 - w / 2;
    if (in.tap_x < x || in.tap_x > x + w) return -1;
    int r = (in.tap_y - y0) / h;
    return in.tap_y >= y0 && r < n ? r : -1;
}

static void move_sel(int *s, int n, int steps) {
    if (!steps) return;
    int was = *s;
    *s = clampi(*s + steps, 0, n - 1);
    if (*s != was) play_sfx(SFX_TICK, 200);
}

/* a loudspeaker: a box, a cone, and as many waves as the level (0-100) earns; a cross at 0 */
static void speaker(float cx, float cy, float k, int level, uint32_t c) {
    float x = cx - 9 * k;
    rect((int)(x - 7 * k), (int)(cy - 4 * k), (int)(6 * k + 1), (int)(8 * k + 1), c, 256);
    float cone[8] = { x - 1.5f * k, cy - 4 * k, x + 6 * k, cy - 10 * k, x + 6 * k, cy + 10 * k, x - 1.5f * k, cy + 4 * k };
    quad(cone, c, 256);
    if (level <= 0) {
        line(cx + 1 * k, cy - 5 * k, cx + 11 * k, cy + 5 * k, 2.2f * k, c, 256);
        line(cx + 1 * k, cy + 5 * k, cx + 11 * k, cy - 5 * k, 2.2f * k, c, 256);
        return;
    }
    int waves = level < 34 ? 1 : level < 67 ? 2 : 3;
    for (int w = 0; w < 3; ++w) {
        float r = (5 + w * 4.5f) * k;
        for (int sgm = 0; sgm < 8; ++sgm) { /* an arc of 100 degrees, in eight strokes */
            float a0 = -0.87f + sgm * 0.2175f, a1 = a0 + 0.2175f;
            line(cx + fcos(a0) * r, cy + fsin(a0) * r, cx + fcos(a1) * r, cy + fsin(a1) * r, 1.8f * k, c,
                 w < waves ? 256 : 70);
        }
    }
}

#define PAUSE_ROWS_Y 104
#define SPEAKER_X (VIEW_W / 2)
#define SPEAKER_Y 266
#define SPEAKER_R 22

static void pause_frame(float dt) {
    static const char *const rows[] = { "Resume", "Restart", "Quit Song" };
    int items = volume_can() ? 4 : 3; /* the fourth: the speaker, for the volume */
    if (g.pause_count > 0) { /* counting back in */
        g.pause_count -= dt;
        draw_play(0);
        char b[8];
        snprintf(b, sizeof b, "%d", (int)(g.pause_count / 0.5f) + 1);
        text_c(F_HUGE, b, VIEW_W / 2, 100, WHITE, 256);
        if (g.pause_count <= 0) {
            playing_input = 1;
            memset(keys_down, 0, sizeof keys_down);
            snd_pause(0);
            state = S_PLAY;
        }
        return;
    }
    move_sel(&g.pause_sel, items, wheel_steps(20) + in.next - in.prev);
    int t = row_tapped(3, PAUSE_ROWS_Y, 44, 200);
    if (items == 4 && in.tap && abs(in.tap_x - SPEAKER_X) <= SPEAKER_R + 6 && abs(in.tap_y - SPEAKER_Y) <= SPEAKER_R + 6) t = 3;
    if (t >= 0) g.pause_sel = t;
    draw_play(0);
    darken(170);
    text_c(F_BIG, "PAUSED", VIEW_W / 2, 52, WHITE, 256);
    menu_rows(rows, 3, g.pause_sel, PAUSE_ROWS_Y, 44, 200);
    if (items == 4) {
        int on = g.pause_sel == 3;
        disc(SPEAKER_X, SPEAKER_Y, SPEAKER_R, on ? ORANGE : 0x000000, on ? 240 : 140);
        if (!on) ring(SPEAKER_X, SPEAKER_Y, SPEAKER_R, SPEAKER_R, 1.5f, 0x5a5478, 256);
        speaker(SPEAKER_X + 2, SPEAKER_Y, 1.0f, volume_level < 0 ? 50 : volume_level, on ? 0x1a0a00 : WHITE);
    }
    if (in.back || in.play) g.pause_sel = 0, in.centre = 1;
    if (in.centre || t >= 0) {
        play_sfx(SFX_SELECT, 200);
        if (g.pause_sel == 0) g.pause_count = 1.5f;
        else if (g.pause_sel == 1) stop_song(), start_song();
        else if (g.pause_sel == 2) stop_song(), state = S_SONG;
        else volume_ask(), g.vol_acc = 0, state = S_VOLUME;
    }
}

/* The volume, from the pause menu: the wheel sets it, or a finger on the bar; Centre, Return or
 * Play/Pause goes back. It is the Q2's own volume, set through Q2 Pod (/tmp/q2-hero.volume). */
#define VBAR_X 72
#define VBAR_W 231
#define VBAR_Y 214
static void volume_frame(void) {
    int was = volume_level;
    g.vol_acc -= in.wheel; /* clockwise: louder */
    int steps = g.vol_acc / 6;
    g.vol_acc -= steps * 6;
    if (steps) volume_level = clampi((volume_level < 0 ? 50 : volume_level) + steps, 0, 100);
    if (in.touch && in.touch_y > VBAR_Y - 30 && in.touch_y < VBAR_Y + 34)
        volume_level = clampi(((in.touch_x - VBAR_X) * 100 + VBAR_W / 2) / VBAR_W, 0, 100);
    if (volume_level != was && volume_level >= 0) {
        volume_set(volume_level);
        if (volume_level / 5 != (was < 0 ? -1 : was / 5)) play_sfx(SFX_TICK, 200);
    }
    draw_play(0);
    darken(220);
    text_c(F_BIG, "VOLUME", VIEW_W / 2, 26, WHITE, 256);
    int v = volume_level < 0 ? 50 : volume_level;
    speaker(VIEW_W / 2 + 3, 102, 2.0f, v, WHITE);
    char b[16];
    if (volume_level >= 0) snprintf(b, sizeof b, "%d", volume_level);
    else snprintf(b, sizeof b, "-");
    text_c(F_BIG, b, VIEW_W / 2, 140, GOLD, 256);
    rrect(VBAR_X, VBAR_Y - 4, VBAR_W, 8, 4, 0x3a3648, 256);
    rrect(VBAR_X, VBAR_Y - 4, 8 + (VBAR_W - 8) * v / 100, 8, 4, ORANGE, 256);
    disc(VBAR_X + 4 + (VBAR_W - 8) * v / 100.0f, VBAR_Y, 11, WHITE, 256);
    text_c(F_SMALL, "Turn the wheel or slide the bar", VIEW_W / 2, 246, GREY, 230);
    text_c(F_SMALL, "Centre: done", VIEW_W / 2, 266, GREY, 230);
    if (in.centre || in.back || in.play || (in.tap && (in.tap_y < VBAR_Y - 30 || in.tap_y > VBAR_Y + 34))) {
        play_sfx(SFX_SELECT, 200);
        g.pause_sel = 3;
        state = S_PAUSE;
    }
}

static void results_frame(float dt) {
    static int sel_r;
    static const char *const rows[] = { "Play Again", "Choose Song" };
    float t = g.end_t += dt;
    gradient(0, VIEW_H, g.failed ? 0x2a0808 : 0x1a0a2e, 0x050308);
    glow(VIEW_W / 2.0f, 40, 160, g.failed ? 0xff2020 : ORANGE, 60 + (int)(fsin(anim * 2) * 20));
    text_c(F_BIG, g.failed ? "SONG FAILED" : g.stars >= 4 ? "YOU ROCK!" : g.stars >= 3 ? "NICE PLAYING!" : "SONG COMPLETE",
           VIEW_W / 2, 18, g.failed ? 0xff5050 : GOLD, 256);
    const char *title = an.info.title[0] ? an.info.title : song_name;
    char b[96];
    snprintf(b, sizeof b, "%s  ·  %s", title, diff_name[diff]);
    int w = text_w(F_SMALL, b);
    if (w > 300) text_fit(F_SMALL, b, VIEW_W / 2 - 150, 56, 300, GREY, 256);
    else text_c(F_SMALL, b, VIEW_W / 2, 56, GREY, 256);
    int shown = (int)(t / 0.35f);
    static int said;
    if (t < dt * 1.5f) said = 0;
    if (!g.failed) {
        stars_row(VIEW_W / 2, 96, shown < g.stars ? shown : g.stars, 5, 13, 256);
        if (shown > said && said < g.stars) play_sfx(SFX_STAR, 200), said = shown;
    }
    int score = (int)(g.score * clampf(t / 1.2f, 0, 1));
    snprintf(b, sizeof b, "%d", score);
    text_c(F_BIG, b, VIEW_W / 2, 116, WHITE, 256);
    if (g.new_best && t > 1.3f) text_c(F_SMALL, "NEW BEST!", VIEW_W / 2, 152, GOLD, (int)(fsin(anim * 6) * 60 + 196));
    int pct = g.notes_total ? g.hits * 100 / g.notes_total : 0;
    char l1[48], l2[48], l3[48];
    snprintf(l1, sizeof l1, "%d%%", pct);
    snprintf(l2, sizeof l2, "%d", g.best_streak);
    snprintf(l3, sizeof l3, "%d / %d", g.phrases_hit, g.phrases);
    const char *lab[3] = { "Notes hit", "Best streak", "Hero phrases" }, *val[3] = { l1, l2, l3 };
    for (int i = 0; i < 3; ++i) {
        int x = 70 + i * 118;
        text_c(F_MED, val[i], x, 174, WHITE, 256);
        text_c(F_SMALL, lab[i], x, 196, GREY, 230);
    }
    if (t < 0.4f) in.centre = in.tap = 0; /* a press left over from playing */
    move_sel(&sel_r, 2, wheel_steps(20) + in.next - in.prev);
    int tp = row_tapped(2, 226, 40, 180);
    if (tp >= 0) sel_r = tp;
    menu_rows(rows, 2, sel_r, 226, 40, 180);
    if (in.back) sel_r = 1, in.centre = 1;
    if (in.centre || tp >= 0) {
        play_sfx(SFX_SELECT, 200);
        if (sel_r == 0) start_song();
        else state = S_SONG;
        sel_r = 0;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Title */
static void demo_highway(void) {
    float now = anim, beats[16];
    float p = 0.5f, b0 = (float)(int)(now / p) * p - 2 * p;
    for (int i = 0; i < 16; ++i) beats[i] = b0 + i * p;
    stage(0.6f + 0.3f * fsin(anim), 0, 0);
    board(now, beats, 16, 0, 0);
    float vis = visible_s();
    for (int k = (int)(now / 0.25f) + (int)(vis / 0.25f) + 1; k >= (int)(now / 0.25f) - 1; --k) {
        unsigned h = (unsigned)k * 2654435761u;
        if (h >> 29 < 4) continue;
        float t = k * 0.25f, dt = t - now;
        if (dt < 0 || dt > vis) continue;
        float z = dt / vis * DEPTH;
        int lane = (int)(h >> 7) % 3;
        gem(lane_x(lane, z), y_of(z), sc_of(z), lane, 0, (int)(clampf((DEPTH - z) / 0.35f, 0, 1) * 256), 0);
    }
    for (int l = 0; l < LANES; ++l) {
        ellipse(lane_x(l, 0), STRIKE_Y + 2, 30, 15, 0x15131c, 256);
        ring(lane_x(l, 0), STRIKE_Y + 2, 30, 15, 4.5f, lane_col[l], 256);
    }
}

static void title_frame(void) {
    static int sel_t;
    static const char *const rows[] = { "Play", "Options", "Quit" };
    demo_highway();
    darken(110);
    glow(VIEW_W / 2.0f, 64, 150, ORANGE, 70 + (int)(fsin(anim * 3) * 25));
    logo((VIEW_W - logo_w) / 2, 16);
    text_c(F_MED, "Bring The Tunes!", VIEW_W / 2, 122, 0xffd24a, 256);
    move_sel(&sel_t, 3, wheel_steps(20) + in.next - in.prev);
    int tp = row_tapped(3, 154, 40, 170);
    if (tp >= 0) sel_t = tp;
    menu_rows(rows, 3, sel_t, 154, 40, 170);
    if (in.centre || tp >= 0) {
        play_sfx(SFX_SELECT, 200);
        if (sel_t == 0) state = S_BROWSE;
        else if (sel_t == 1) state = S_OPTIONS;
        else state = -1;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Songs */
static void browse_frame(float dt) {
    int rows = 8, rh = 30, y0 = 50;
    if (nents) {
        int was = sel;
        sel = clampi(sel + wheel_steps(18) + in.next - in.prev, 0, nents - 1);
        if (sel != was) play_sfx(SFX_TICK, 160);
    }
    if (in.drag) scroll_y = clampf(scroll_y - in.drag / (float)rh, 0, (float)(nents > rows ? nents - rows : 0)), top_row = (int)(scroll_y + 0.5f);
    if (sel < top_row) top_row = sel;
    if (sel >= top_row + rows) top_row = sel - rows + 1;
    if (!in.touch) scroll_y += (top_row - scroll_y) * minf(1, dt * 14);
    int open = in.centre;
    if (in.tap && in.tap_y >= y0) {
        int r = (int)((in.tap_y - y0) / rh + scroll_y);
        if (r >= 0 && r < nents) sel = r, open = 1;
    }
    gradient(0, VIEW_H, 0x1a0a2e, 0x07040c);
    /* the header */
    const char *here = strrchr(cwd, '/');
    int at_root = !strcmp(cwd, music_root());
    rect(0, 0, VIEW_W, 44, 0x000000, 120);
    text_c(F_MED, at_root ? "Songs" : here ? here + 1 : cwd, VIEW_W / 2, 12, WHITE, 256);
    clip(0, 44, VIEW_W, VIEW_H);
    if (!nents) text_c(F_MED, "No songs here", VIEW_W / 2, 140, GREY, 256);
    for (int i = (int)scroll_y - 1; i < (int)scroll_y + rows + 1; ++i) {
        if (i < 0 || i >= nents) continue;
        float y = y0 + (i - scroll_y) * rh;
        int x0 = 26, w = VIEW_W - 52;
        if (i == sel) {
            rrect(x0 - 6, (int)y + 1, w + 12, rh - 2, 12, ORANGE, 235);
            rrect(x0 - 6, (int)y + 1, w + 12, (rh - 2) / 2, 12, 0xffb050, 90);
        }
        uint32_t c = i == sel ? 0x1a0a00 : WHITE;
        if (ents[i].dir) icon_folder(x0, y + 7, i == sel ? 0x3a1a00 : 0xf0b040);
        else icon_note(x0 + 2, y + 6, i == sel ? 0x3a1a00 : 0xff7aa8);
        char name[256];
        const char *artist = "";
        if (ents[i].tagged && ents[i].title[0]) { /* the song's own title, its artist after it */
            snprintf(name, sizeof name, "%s", ents[i].title);
            artist = ents[i].artist;
        } else {
            snprintf(name, sizeof name, "%s", ents[i].name);
            char *dot = ents[i].dir ? 0 : strrchr(name, '.');
            if (dot && dot != name) *dot = 0;
        }
        int right = ents[i].stars >= 0 ? 62 : 6, room = w - 28 - right;
        int tw = text_w(F_MED, name), aw = artist[0] ? text_w(F_SMALL, artist) : 0;
        if (aw && tw + 10 + aw > room) tw = tw < room * 2 / 3 ? tw : room * 2 / 3; /* the title keeps two thirds */
        text_fit(F_MED, name, x0 + 28, (int)y + 3, artist[0] ? tw : room, c, 256);
        if (artist[0] && room - tw - 10 > 24)
            text_fit(F_SMALL, artist, x0 + 28 + tw + 10, (int)y + 7, room - tw - 10, i == sel ? 0x4a2a00 : GREY, 256);
        if (ents[i].stars >= 0)
            for (int s = 0; s < 5; ++s)
                star(x0 + w - 52 + s * 11.0f, y + rh / 2.0f, 5, s < ents[i].stars ? (i == sel ? 0x3a1a00 : GOLD) : (i == sel ? 0xd07010 : 0x3a3648), 256);
    }
    clip_reset();
    if (nents > rows) { /* where in the list */
        float h = 240.0f * rows / nents, y = 50 + (240 - h) * scroll_y / (nents - rows);
        rrect(VIEW_W - 12, (int)y, 4, (int)h, 2, 0x8a84a0, 180);
    }
    if (in.back) {
        play_sfx(SFX_BACK, 200);
        if (at_root) state = S_TITLE;
        else {
            char up[512], name[256];
            snprintf(up, sizeof up, "%s", cwd);
            char *s = strrchr(up, '/');
            snprintf(name, sizeof name, "%s", s ? s + 1 : "");
            if (s) *s = 0;
            open_dir(up, name);
        }
        return;
    }
    if (open && nents) {
        play_sfx(SFX_SELECT, 200);
        char p[800];
        join(p, sizeof p, cwd, ents[sel].name);
        if (ents[sel].dir) open_dir(p, 0);
        else {
            snprintf(set.last_dir, sizeof set.last_dir, "%s", cwd);
            settings_save();
            choose_song(p, ents[sel].tagged && ents[sel].title[0] ? ents[sel].title : ents[sel].name);
        }
    }
}

static void listen_frame(void) {
    gradient(0, VIEW_H, 0x1a0a2e, 0x07040c);
    text_c(F_BIG, "LISTENING", VIEW_W / 2, 30, GOLD, 256);
    text_fit(F_MED, song_name, VIEW_W / 2 - min_i(text_w(F_MED, song_name), 300) / 2, 76, 300, WHITE, 256);
    /* what it hears, in five bands */
    for (int b = 0; b < 5; ++b) {
        float v = listen_meter(job.l, b), h = 10 + v * 70;
        uint32_t c = mix(0xff3a10, 0xffd24a, b * 60);
        rrect(118 + b * 28, (int)(196 - h), 20, (int)h, 4, c, 230);
    }
    int pct = (int)(job.progress * 100);
    rrect(70, 214, 235, 12, 6, 0x2a2638, 230);
    rrect(70, 214, 12 + (int)(223 * job.progress), 12, 6, ORANGE, 240);
    char b[64];
    snprintf(b, sizeof b, "%d%%", pct);
    text_c(F_SMALL, b, VIEW_W / 2, 232, GREY, 230);
    text_c(F_SMALL, "Finding the tune, the bass and the beat.", VIEW_W / 2, 258, GREY, 200);
    text_c(F_SMALL, "Once a song: it's kept for next time.", VIEW_W / 2, 276, GREY, 200);
    if (in.back) job.cancel = 1;
    if (job.done) {
        pthread_join(job.thread, 0);
        job.running = 0;
        if (job.result == 0) {
            have_an = 1;
            for (int d = 0; d < DIFFS; ++d) {
                Chart c;
                diff_notes[d] = chart_make(&an, d, &c) ? c.count : 0;
                chart_free(&c);
            }
            state = S_SONG;
            play_sfx(SFX_SELECT, 200);
        } else if (job.result == 1) {
            drop_listener();
            state = S_BROWSE;
        } else {
            drop_listener();
            show_note("Can't read this song", "The Q2's ffmpeg couldn't decode it, or it has no beat to follow. "
                                              "Try another song.", S_BROWSE);
        }
    }
}

static void song_frame(void) {
    if (!have_an) {
        state = S_BROWSE;
        return;
    }
    move_sel(&diff, DIFFS, wheel_steps(20) + in.next - in.prev);
    gradient(0, VIEW_H, 0x1a0a2e, 0x07040c);
    const char *title = an.info.title[0] ? an.info.title : song_name;
    int tw = min_i(text_w(F_MED, title), 310);
    text_fit(F_MED, title, VIEW_W / 2 - tw / 2, 16, 310, WHITE, 256);
    char b[160];
    int secs = (int)an.duration;
    if (an.info.artist[0]) snprintf(b, sizeof b, "%s  ·  %d:%02d  ·  %d BPM", an.info.artist, secs / 60, secs % 60, (int)(an.bpm + 0.5f));
    else snprintf(b, sizeof b, "%d:%02d  ·  %d BPM", secs / 60, secs % 60, (int)(an.bpm + 0.5f));
    int bw = min_i(text_w(F_SMALL, b), 310);
    text_fit(F_SMALL, b, VIEW_W / 2 - bw / 2, 42, 310, GREY, 240);
    int y0 = 74, h = 48, x0 = 44, w = VIEW_W - 88;
    int tp = -1;
    if (in.tap && in.tap_y >= y0 && in.tap_y < y0 + DIFFS * h) tp = (in.tap_y - y0) / h, diff = tp;
    for (int d = 0; d < DIFFS; ++d) {
        int y = y0 + d * h;
        int on = d == diff;
        rrect(x0, y, w, h - 6, 14, on ? ORANGE : 0x000000, on ? 240 : 120);
        if (on) rrect(x0, y, w, (h - 6) / 2, 14, 0xffb050, 90);
        uint32_t c = on ? 0x1a0a00 : WHITE;
        text(F_MED, diff_name[d], x0 + 16, y + 5, c, 256);
        snprintf(b, sizeof b, "%d notes", diff_notes[d]);
        text(F_SMALL, b, x0 + 16, y + 24, on ? 0x4a2a00 : GREY, 230);
        Best best = best_get(song_id, d);
        if (best.score) {
            for (int s = 0; s < 5; ++s) star(x0 + w - 70 + s * 13.0f, y + 13.0f, 6, s < best.stars ? (on ? 0x3a1a00 : GOLD) : (on ? 0xd07010 : 0x3a3648), 256);
            snprintf(b, sizeof b, "%d", best.score);
            text_r(F_SMALL, b, x0 + w - 14, y + 23, on ? 0x4a2a00 : GREY, 230);
        }
    }
    text_c(F_SMALL, "Centre: play   ·   Return: back", VIEW_W / 2, 270, GREY, 200);
    if (in.back) {
        play_sfx(SFX_BACK, 200);
        open_dir(cwd, strrchr(song_path, '/') ? strrchr(song_path, '/') + 1 : 0);
        state = S_BROWSE;
        return;
    }
    if (in.centre || tp >= 0) {
        play_sfx(SFX_SELECT, 200);
        set.last_diff = diff;
        settings_save();
        start_song();
    }
}

/* ---------------------------------------------------------------------------------------------
 * Options and the timing check */
static struct {
    int sel, editing;
} opt;

static struct {
    int taps, beat_next, done;
    uint64_t done_at;
    float delta[32];
    int result_ms;
} cal;
#define CAL_BEAT 0.5f
#define CAL_BEATS 28

static void calib_press(void) {
    if (cal.done || state != S_CALIBRATE) return;
    uint64_t at = event_time(), now = now_us();
    float t = (float)snd_time() - (float)(now > at ? now - at : 0) / 1e6f;
    int k = (int)(t / CAL_BEAT + 0.5f);
    if (k < 2 || cal.taps >= 32) return;
    float d = t - k * CAL_BEAT;
    if (fabs_(d) > 0.4f) return;
    cal.delta[cal.taps++] = d;
}

static int cmp_float(const void *a, const void *b) {
    float d = *(const float *)a - *(const float *)b;
    return d < 0 ? -1 : d > 0;
}

static void calibrate_start(void) {
    memset(&cal, 0, sizeof cal);
    snd_music(MUS_SILENT, RATE);
    state = S_CALIBRATE;
}

static void calibrate_frame(void) {
    float t = (float)snd_time();
    g.now = t, g.now_us = now_us(); /* for the tests' clock */
    /* a press that was a tap, or one just after the last, isn't an answer */
    int was_done = cal.done && now_us() - cal.done_at > 800000 ? cal.done : 0;
    /* the clicks a second ahead */
    while (!cal.done && cal.beat_next < CAL_BEATS && cal.beat_next * CAL_BEAT < t + 1) {
        snd_play_at(&sfx[SFX_CLICK], 256, 0, (int64_t)(cal.beat_next * CAL_BEAT * RATE));
        ++cal.beat_next;
    }
    if (!cal.done && (cal.taps >= 16 || t > CAL_BEATS * CAL_BEAT + 0.5f)) {
        cal.done = 1;
        cal.done_at = now_us();
        snd_music(MUS_OFF, 0);
        if (cal.taps >= 6) {
            float s[32];
            memcpy(s, cal.delta, sizeof s);
            qsort(s, (size_t)cal.taps, sizeof *s, cmp_float);
            cal.result_ms = (int)(s[cal.taps / 2] * 1000 + (s[cal.taps / 2] >= 0 ? 0.5f : -0.5f));
        } else
            cal.done = 2;
    }
    gradient(0, VIEW_H, 0x1a0a2e, 0x07040c);
    text_c(F_BIG, "TIMING", VIEW_W / 2, 20, GOLD, 256);
    float ph = t / CAL_BEAT - (float)(int)(t / CAL_BEAT);
    if (t < 0) ph = 1;
    if (!cal.done) {
        text_c(F_SMALL, "Press Centre on each click, in time with it.", VIEW_W / 2, 66, WHITE, 240);
        float r = 30 + (1 - clampf(ph * 3, 0, 1)) * 14;
        disc(VIEW_W / 2.0f, 160, r, mix(0x2a2638, ORANGE, (int)((1 - clampf(ph * 3, 0, 1)) * 256)), 256);
        ring(VIEW_W / 2.0f, 160, 46, 46, 3, 0x5a5478, 256);
        char b[48];
        snprintf(b, sizeof b, "%d of 16", cal.taps);
        text_c(F_SMALL, b, VIEW_W / 2, 222, GREY, 230);
    } else if (cal.done == 1) {
        char b[64];
        snprintf(b, sizeof b, "%+d ms", cal.result_ms);
        text_c(F_HUGE, b, VIEW_W / 2, 90, WHITE, 256);
        text_c(F_SMALL, cal.result_ms > 0 ? "Your presses come after the sound by this much." : "Your presses come before the sound by this much.",
               VIEW_W / 2, 170, GREY, 230);
        text_c(F_SMALL, "Centre: keep it   ·   Return: don't", VIEW_W / 2, 250, WHITE, 230);
    } else {
        text_c(F_MED, "Not enough presses", VIEW_W / 2, 120, WHITE, 256);
        text_c(F_SMALL, "Centre: try again   ·   Return: back", VIEW_W / 2, 250, WHITE, 230);
    }
    if (in.back) {
        snd_music(MUS_OFF, 0);
        state = S_OPTIONS;
        play_sfx(SFX_BACK, 200);
    } else if (in.centre && was_done == 1) {
        *offset_for_output() = clampi(cal.result_ms, -300, 600);
        settings_save();
        state = S_OPTIONS;
        play_sfx(SFX_SELECT, 200);
    } else if (in.centre && was_done == 2)
        calibrate_start();
}

static void options_frame(void) {
    enum { O_SPEED, O_LEFTY, O_FAIL, O_SFX, O_OFFSET, O_CALIBRATE, O_BACK, O_N };
    static const char *const label[O_N] = { "Note speed", "Lefty flip", "Fail", "Effects", "Timing", "Check timing", "Back" };
    int steps = wheel_steps(20) + in.next - in.prev;
    int *off = offset_for_output();
    if (opt.editing) {
        if (steps) *off = clampi(*off + steps * 5, -300, 600), play_sfx(SFX_TICK, 160);
    } else
        move_sel(&opt.sel, O_N, steps);
    int y0 = 52, h = 34, x0 = 40, w = VIEW_W - 80;
    int tp = -1;
    if (in.tap && in.tap_y >= y0 && in.tap_y < y0 + O_N * h) tp = (in.tap_y - y0) / h, opt.sel = tp;
    gradient(0, VIEW_H, 0x1a0a2e, 0x07040c);
    text_c(F_BIG, "OPTIONS", VIEW_W / 2, 8, GOLD, 256);
    for (int i = 0; i < O_N; ++i) {
        int y = y0 + i * h, on = i == opt.sel;
        rrect(x0, y, w, h - 5, 12, on ? (opt.editing ? 0xffd24a : ORANGE) : 0, on ? 240 : 110);
        uint32_t c = on ? 0x1a0a00 : WHITE;
        text(F_MED, label[i], x0 + 14, y + 4, c, 256);
        char v[40] = "";
        static const char *const sfxn[3] = { "Off", "Low", "High" };
        if (i == O_SPEED) snprintf(v, sizeof v, "%d", set.speed);
        if (i == O_LEFTY) snprintf(v, sizeof v, "%s", set.lefty ? "On" : "Off");
        if (i == O_FAIL) snprintf(v, sizeof v, "%s", set.nofail ? "No Fail" : "On");
        if (i == O_SFX) snprintf(v, sizeof v, "%s", sfxn[set.sfx]);
        if (i == O_OFFSET) snprintf(v, sizeof v, "%+d ms", *off);
        text_r(F_MED, v, x0 + w - 14, y + 4, c, 256);
    }
    const char *hint = opt.editing ? "Turn the wheel, then Centre" :
                       opt.sel == O_OFFSET ? "How late your presses count: Centre, then the wheel" :
                       opt.sel == O_LEFTY ? "Next on the left, Prev on the right" :
                       opt.sel == O_SPEED ? "How fast the notes come down" :
                       opt.sel == O_CALIBRATE ? "Press along to clicks to set Timing" : "";
    text_c(F_SMALL, hint, VIEW_W / 2, 296, GREY, 220);
    if (in.back) {
        if (opt.editing) opt.editing = 0;
        else settings_save(), state = S_TITLE;
        play_sfx(SFX_BACK, 200);
        return;
    }
    if (in.centre || tp >= 0) {
        play_sfx(SFX_SELECT, 180);
        switch (opt.sel) {
        case O_SPEED: set.speed = set.speed % 5 + 1; break;
        case O_LEFTY: set.lefty = !set.lefty; break;
        case O_FAIL: set.nofail = !set.nofail; break;
        case O_SFX: set.sfx = (set.sfx + 1) % 3; break;
        case O_OFFSET: opt.editing = !opt.editing; break;
        case O_CALIBRATE: settings_save(), calibrate_start(); break;
        case O_BACK: settings_save(), state = S_TITLE; break;
        }
    }
}

static void note_frame(void) {
    gradient(0, VIEW_H, 0x1a0a2e, 0x07040c);
    text_c(F_BIG, note_title, VIEW_W / 2, 70, GOLD, 256);
    /* the body, wrapped */
    char line_b[128];
    const char *p = note_body;
    int y = 130;
    while (*p && y < 250) {
        int n = 0, cut = 0;
        while (p[n]) {
            snprintf(line_b, sizeof line_b, "%.*s", n + 1, p);
            if (text_w(F_SMALL, line_b) > 290) break;
            if (p[n] == ' ') cut = n;
            ++n;
        }
        if (p[n] && cut) n = cut;
        snprintf(line_b, sizeof line_b, "%.*s", n, p);
        text_c(F_SMALL, line_b, VIEW_W / 2, y, WHITE, 240);
        p += n;
        while (*p == ' ') ++p;
        y += 20;
    }
    text_c(F_SMALL, "Centre or Return", VIEW_W / 2, 270, GREY, 200);
    if (in.centre || in.back || in.tap) state = note_back, play_sfx(SFX_BACK, 200);
}

/* ---------------------------------------------------------------------------------------------
 * For the host tests: the state each frame, and the chart */
static void write_state(void) {
    static int on = -1;
    if (on < 0) on = getenv("HERO_STATE") != 0;
    if (!on) return;
    FILE *f = fopen("state.txt.new", "w");
    if (!f) return;
    fprintf(f, "state %d\nsel %d\nnents %d\ndiff %d\nscore %d\nhits %d\nmisses %d\nnotes %d\nstreak %d\nbest_streak %d\n"
               "now_ms %d\nrock %d\nhero %d\nhero_on %d\nstars %d\nlisten %d\nhave_an %d\nbpm10 %d\nnonsets %d\nspeed %d\n"
               "offset %d\nvolume %d\nnow_us %lld\nfailed %d\nopt %d\ncal_taps %d\ncal_ms %d\n",
            state, state == S_BROWSE ? sel : state == S_SONG ? diff : 0, nents, diff, g.score, g.hits, g.misses,
            g.notes_total, g.streak, g.best_streak, (int)(g.now * 1000), (int)(g.rock * 100), (int)(g.hero * 100),
            g.hero_on, g.stars, (int)(job.progress * 100), have_an, (int)(an.bpm * 10), an.nonsets, set.speed,
            *offset_for_output(), volume_level, (long long)g.now_us, g.failed, opt.sel, cal.taps, cal.result_ms);
    fclose(f);
    rename("state.txt.new", "state.txt");
    static Note *written;
    if (state == S_PLAY && chart.n != written) { /* the notes, for the test to play */
        written = chart.n;
        FILE *c = fopen("chart.txt", "w");
        for (int i = 0; c && i < chart.count; ++i) fprintf(c, "%.4f %d %.3f %d\n", chart.n[i].t, chart.n[i].lanes, chart.n[i].len, chart.n[i].hero);
        if (c) fclose(c);
    }
}

int game_main(void) {
    settings_load();
    sfx_make();
    volume_ask();
    rnd();
    const char *root = music_root();
    DIR *d = set.last_dir[0] && !strncmp(set.last_dir, root, strlen(root)) ? opendir(set.last_dir) : 0;
    if (d) closedir(d), open_dir(set.last_dir, 0);
    else open_dir(root, 0);
    diff = set.last_diff;
    uint64_t last = now_us();
    last_input_t = last;
    frame_t = last;
    while (!quit_asked() && state >= 0) {
        uint64_t t = now_us();
        float dt = clampf((float)(t - last) / 1e6f, 0, 0.1f);
        last = t, anim += dt;
        int was = state;
        input_frame();
        int busy = state == S_PLAY || state == S_PAUSE || state == S_VOLUME || state == S_LISTEN || state == S_CALIBRATE;
        if (!busy && t > last_input_t && t - last_input_t > IDLE_QUIT_US) {
            logf_("idle: closing");
            break;
        }
        switch (state) {
        case S_TITLE: title_frame(); break;
        case S_BROWSE: browse_frame(dt); break;
        case S_LISTEN: listen_frame(); break;
        case S_SONG: song_frame(); break;
        case S_PLAY: play_frame(dt); break;
        case S_PAUSE: pause_frame(dt); break;
        case S_RESULTS: results_frame(dt); break;
        case S_OPTIONS: options_frame(); break;
        case S_CALIBRATE: calibrate_frame(); break;
        case S_NOTE: note_frame(); break;
        case S_VOLUME: volume_frame(); break;
        }
        if (state != was) wheel_acc = 0, logf_("screen %d -> %d", was, state);
        if (state < 0) break;
        present(fb);
        write_state();
        frame_t += FPS_US;
        uint64_t after = now_us();
        if (frame_t > after) sleep_us((unsigned)(frame_t - after));
        else if (after - frame_t > 100000) frame_t = after;
    }
    stop_song();
    job_end();
    tagger_stop();
    tags_save();
    settings_save();
    return 0;
}
