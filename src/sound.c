/* Q2 Hero's sound: the song, streamed into a ring by decode.c, and the game's effects, mixed
 * here and played through ALSA the way Q2 Pod's own q2video plays (patch/video.c sound_open,
 * docs/librespot.md), as Challenger Deep does:
 *
 *   - Q2 Pod passes the output as argv[1]: "plughw:0,0" (the headphone DAC), "plug:bluealsa" or
 *     "usb" (the USB-Audio card in /proc/asound/cards), and off the jack argv[2], the volume:
 *     "NN" (0-100, applied here as hciplayer's Bluetooth curve) or "hNN" (the DAC applies it).
 *   - hciplayer lets go of the PCM a moment after Q2 Pod stops it, so the open is retried.
 *   - On the jack, the DAC's PCM mode and mute are cleared through /dev/shanling_dac after the
 *     open, and the mute set again on close.
 *   - Mono Audio and Swap L/R follow /tmp/q2mono and /tmp/q2swap.
 *
 * The clock. The game is drawn and judged against the music as it is heard: after each write,
 * snd_pcm_delay says how many frames are still queued ahead of the speaker, and a short history
 * of which music frame each written chunk began with turns that into the music frame heard now.
 * Pauses and a ring that runs dry don't move the music, so they don't move the clock. With no
 * device the same loop runs against the wall clock, silently. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "hero.h"

#define CHUNK 256 /* frames a write, about 6 ms */
#define CHANNELS 16
#define LATENCY_JACK 50000
#define LATENCY_OTHER 200000 /* Bluetooth needs the slack, as q2video gives it */
#define DAC "/dev/shanling_dac"
#define DAC_PCM 0xc0044d1bu
#define DAC_MUTE 0xc0044d1fu
#define MONO_FILE "/tmp/q2mono"
#define SWAP_FILE "/tmp/q2swap"
#define RING (1u << 17) /* frames: 3 s */
#define HIST 256        /* chunks remembered for the clock: 1.5 s, more than any output's delay */

/* alsa-lib, as q2video declares it */
int snd_pcm_open(void **, const char *, int, int), snd_pcm_close(void *), snd_pcm_drop(void *);
int snd_pcm_set_params(void *, int, int, unsigned, unsigned, int, unsigned);
int snd_pcm_recover(void *, int, int);
int snd_pcm_delay(void *, long *);
long snd_pcm_writei(void *, const void *, unsigned long);
const char *snd_strerror(int);
int pthread_timedjoin_np(pthread_t, void **, const struct timespec *);

/* ---------------------------------------------------------------------------------------------
 * The ring: one writer (decode.c's thread), one reader (the mixer) */
static int16_t rbuf[RING * 2];
static volatile unsigned r_head, r_tail; /* frames, free-running */
static volatile int r_eof;

unsigned ring_space(void) { return RING - (r_head - r_tail); }
void ring_put(const int16_t *f, unsigned n) {
    unsigned h = r_head;
    for (unsigned i = 0; i < n; ++i) {
        unsigned at = (h + i) % RING;
        rbuf[2 * at] = f[2 * i], rbuf[2 * at + 1] = f[2 * i + 1];
    }
    __sync_synchronize();
    r_head = h + n;
}
void ring_reset(void) { r_tail = r_head, r_eof = 0; }
void ring_eof(int eof) {
    __sync_synchronize();
    r_eof = eof;
}

/* ---------------------------------------------------------------------------------------------
 * State */
static struct {
    const int16_t *data;
    unsigned len, pos;
    int left, right; /* 0-256 */
    int64_t at;      /* the music frame it starts on, when it waits */
    int waits;
} chan[CHANNELS];

static struct {
    char device[32];
    int gain; /* 1/65536; 65536 where the DAC applies the volume */
    void *pcm;
    int dac; /* /dev/shanling_dac on the jack, -1 */
    pthread_t thread;
    int running, null_out;
    volatile int stop;
    volatile int *mono_on, *swap_on;
    /* the music */
    volatile int mode, paused, ended;
    int64_t pos;         /* the next music frame to mix */
    int64_t from;        /* where this music began: -lead */
    int music_vol, sfx_vol;
    float muffle;        /* 0..1, how dull */
    float lp[2];
    /* the clock: written by the mixer under clock_lock */
    double heard;        /* the music frame heard at stamp */
    uint64_t stamp;
    int moving;          /* the music was moving then */
    uint64_t out_total;  /* frames written to the device */
    struct {
        uint64_t out;  /* out_total when the chunk began */
        int64_t pos;   /* the music frame then */
        int moving;
    } hist[HIST];
    unsigned nhist;
} snd = { .dac = -1, .music_vol = 256, .sfx_vol = 200 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER, clock_lock = PTHREAD_MUTEX_INITIALIZER;

/* hciplayer's Bluetooth soft volume for Q2 Pod's v (0-100), as q2video's bt_gain:
 * 0.002 v under 50, then 0.1 + 0.018 (v - 50), 1 at 100. "hNN", or none, is unity. */
static int gain_for(const char *volume) {
    if (!volume || volume[0] == 'h') return 65536;
    int v = atoi(volume);
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    return 65536 * (v < 50 ? 2 * v : 18 * v - 800) / 1000;
}
void snd_gain_arg(const char *volume) {
    if (!volume || !volume[0]) return;
    if (!strcmp(snd.device, "plughw:0,0")) return; /* the DAC has it on the jack */
    snd.gain = gain_for(volume);
}

/* ---------------------------------------------------------------------------------------------
 * Mixing */
static void mix_out(int16_t *out, unsigned frames) {
    static int acc[2 * CHUNK];
    static int64_t mp[CHUNK]; /* the music frame each output frame carries, -1 none */
    memset(acc, 0, sizeof acc);
    int mode = snd.mode, paused = snd.paused;
    int64_t first = snd.pos;
    int moved_all = 1;
    for (unsigned f = 0; f < frames; ++f) {
        mp[f] = -0x7fffffffffffll;
        if (mode == MUS_OFF || paused) {
            moved_all = 0;
            continue;
        }
        if (snd.pos < 0 || mode == MUS_SILENT) {
            mp[f] = snd.pos++;
            continue;
        }
        if (r_head != r_tail) {
            __sync_synchronize();
            unsigned at = r_tail % RING;
            int l = rbuf[2 * at], r = rbuf[2 * at + 1];
            r_tail = r_tail + 1;
            if (snd.muffle > 0.002f) {
                snd.lp[0] += (l - snd.lp[0]) * 0.12f, snd.lp[1] += (r - snd.lp[1]) * 0.12f;
                l = (int)(l + (snd.lp[0] - l) * snd.muffle), r = (int)(r + (snd.lp[1] - r) * snd.muffle);
                snd.muffle *= 0.99985f;
            } else
                snd.lp[0] = (float)l, snd.lp[1] = (float)r;
            acc[2 * f] = l * snd.music_vol, acc[2 * f + 1] = r * snd.music_vol;
            mp[f] = snd.pos++;
        } else {
            if (r_eof) snd.ended = 1;
            moved_all = 0; /* dry: the music waits for the decoder */
        }
    }
    for (int c = 0; c < CHANNELS; ++c) {
        if (!chan[c].data) continue;
        unsigned f = 0;
        if (chan[c].waits) { /* for its frame of the music */
            while (f < frames && mp[f] < chan[c].at) ++f;
            if (f == frames) {
                if (mode == MUS_OFF) chan[c].data = 0;
                continue;
            }
            if (mp[f] > chan[c].at + RATE / 10) { /* far too late: drop it */
                chan[c].data = 0;
                continue;
            }
            chan[c].waits = 0;
        }
        const int16_t *d = chan[c].data;
        unsigned pos = chan[c].pos, len = chan[c].len;
        int l = chan[c].left * snd.sfx_vol >> 8, r = chan[c].right * snd.sfx_vol >> 8;
        for (; f < frames && pos < len; ++f, ++pos) acc[2 * f] += d[pos] * l, acc[2 * f + 1] += d[pos] * r;
        chan[c].pos = pos;
        if (pos >= len) chan[c].data = 0;
    }
    for (unsigned i = 0; i < 2 * frames; ++i) {
        int v = acc[i] >> 8;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    /* this chunk in the clock's history */
    pthread_mutex_lock(&clock_lock);
    unsigned h = snd.nhist++ % HIST;
    snd.hist[h].out = snd.out_total, snd.hist[h].pos = first, snd.hist[h].moving = moved_all && mode != MUS_OFF;
    pthread_mutex_unlock(&clock_lock);
}

/* the music frame heard now: out_total less the device's delay, found in the history */
static void clock_update(long delay) {
    pthread_mutex_lock(&clock_lock);
    uint64_t heard_out = snd.out_total > (uint64_t)delay ? snd.out_total - (uint64_t)delay : 0;
    double heard = (double)snd.from; /* not yet reached the speaker */
    int moving = 0;
    for (unsigned k = 0; k < HIST && k < snd.nhist; ++k) {
        unsigned h = (snd.nhist - 1 - k) % HIST;
        if (snd.hist[h].out <= heard_out) {
            uint64_t into = heard_out - snd.hist[h].out;
            heard = (double)snd.hist[h].pos + (snd.hist[h].moving ? (double)(into < CHUNK ? into : CHUNK) : 0);
            moving = snd.hist[h].moving;
            break;
        }
    }
    snd.heard = heard, snd.stamp = now_us(), snd.moving = moving;
    pthread_mutex_unlock(&clock_lock);
}

double snd_time(void) {
    pthread_mutex_lock(&clock_lock);
    double f = snd.heard;
    if (snd.moving) {
        double ahead = (double)(now_us() - snd.stamp) * RATE / 1e6;
        f += ahead < 2 * CHUNK ? ahead : 2 * CHUNK;
    }
    pthread_mutex_unlock(&clock_lock);
    return f / RATE;
}
int64_t snd_written(void) { return snd.pos; }

/* ---------------------------------------------------------------------------------------------
 * The device */

/* /tmp/q2mono and /tmp/q2swap: Q2 Pod's ints, mapped once they exist */
static volatile int *map_flag(volatile int *have, const char *path) {
    if (have) return have;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    void *p = mmap(0, sizeof(int), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? 0 : p;
}

/* "usb": the card /proc/asound/cards lists as USB-Audio, as plughw:N,0 */
static void usb_device(char *out, unsigned size) {
    char b[1024];
    int fd = open("/proc/asound/cards", O_RDONLY | O_CLOEXEC), n = fd >= 0 ? (int)read(fd, b, sizeof b - 1) : 0;
    if (fd >= 0) close(fd);
    b[n > 0 ? n : 0] = 0;
    char *u = strstr(b, "USB-Audio");
    while (u && u > b && u[-1] != '\n') --u; /* the line's start: " N [id ]: USB-Audio" */
    snprintf(out, size, "plughw:%d,0", u ? atoi(u) : 1);
}

static int sound_open(void) {
    char dev[32];
    int err = 0, off = 0;
    if (!strcmp(snd.device, "usb")) usb_device(dev, sizeof dev);
    else snprintf(dev, sizeof dev, "%s", snd.device);
    int jack = !strcmp(dev, "plughw:0,0");
    for (int i = 0; i < (jack ? 20 : 50) && !snd.stop && (err = snd_pcm_open(&snd.pcm, dev, 0, 0)); ++i)
        snd.pcm = 0, usleep(100000);
    if (!snd.pcm) {
        fprintf(stderr, "Sound: can't open %s: %s\n", dev, snd_strerror(err));
        return 0;
    }
    /* S16_LE, RW_INTERLEAVED, stereo, resampled as needed; bluealsa claims its PCM here */
    for (int i = 0; (err = snd_pcm_set_params(snd.pcm, 2, 3, 2, RATE, 1, jack ? LATENCY_JACK : LATENCY_OTHER)) == -EBUSY &&
                    !jack && i < 50 && !snd.stop;
         ++i)
        usleep(100000);
    if (err) {
        fprintf(stderr, "Sound: %s: %s\n", dev, snd_strerror(err));
        snd_pcm_close(snd.pcm);
        snd.pcm = 0;
        return 0;
    }
    if (jack && (snd.dac = open(DAC, O_RDWR | O_CLOEXEC)) >= 0)
        ioctl(snd.dac, DAC_PCM, &off), ioctl(snd.dac, DAC_MUTE, &off);
    printf("Sound: %s at %d Hz\n", dev, RATE);
    return 1;
}

static void *mixer(void *unused) {
    (void)unused;
    int16_t buf[2 * CHUNK];
    if (!snd.null_out && !sound_open()) {
        printf("Sound: none, the clock runs silent\n");
        snd.null_out = 1;
    }
    uint64_t next = now_us();
    for (unsigned n = 0; !snd.stop; ++n) {
        if (!(n % 200)) snd.mono_on = map_flag(snd.mono_on, MONO_FILE), snd.swap_on = map_flag(snd.swap_on, SWAP_FILE);
        pthread_mutex_lock(&lock);
        mix_out(buf, CHUNK);
        pthread_mutex_unlock(&lock);
        int gain = snd.gain, mono = snd.mono_on && *snd.mono_on, swap = snd.swap_on && *snd.swap_on;
        for (unsigned i = 0; i < 2 * CHUNK; i += 2) {
            int l = buf[i] * gain >> 16, r = buf[i + 1] * gain >> 16;
            if (mono) l = r = (l + r) >> 1;
            buf[i] = (int16_t)(swap ? r : l), buf[i + 1] = (int16_t)(swap ? l : r);
        }
        long delay = 0;
        if (snd.null_out) { /* the wall clock's pace */
            next += (uint64_t)CHUNK * 1000000 / RATE;
            uint64_t t = now_us();
            if (next > t) usleep((unsigned)(next - t));
            else if (t - next > 100000) next = t;
            snd.out_total += CHUNK;
        } else {
            long w = snd_pcm_writei(snd.pcm, buf, CHUNK);
            if (w < 0) {
                if (snd_pcm_recover(snd.pcm, (int)w, 1) < 0) usleep(20000);
            } else
                snd.out_total += (uint64_t)w;
            if (snd_pcm_delay(snd.pcm, &delay) < 0 || delay < 0) delay = 0;
        }
        clock_update(delay);
    }
    return 0;
}

void snd_start(const char *device, const char *volume) {
    snd.null_out = !(device && device[0] && strcmp(device, "-"));
    snprintf(snd.device, sizeof snd.device, "%s", device && !snd.null_out ? device : "");
    snd.gain = gain_for(volume);
    snd.stop = 0;
    snd.running = !pthread_create(&snd.thread, 0, mixer, 0);
}

int snd_on(void) { return snd.running && !snd.null_out; }

void snd_shutdown(void) {
    if (!snd.running) return;
    snd.stop = 1;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += 1;
    /* an output that takes no sound can hold the writer in ALSA: leave it, the process ends */
    if (pthread_timedjoin_np(snd.thread, 0, &t)) return;
    snd.running = 0;
    if (snd.pcm) snd_pcm_drop(snd.pcm), snd_pcm_close(snd.pcm), snd.pcm = 0;
    int on = 1;
    if (snd.dac >= 0) ioctl(snd.dac, DAC_MUTE, &on), close(snd.dac), snd.dac = -1;
}

/* ---------------------------------------------------------------------------------------------
 * The game's side */

static void start(const Sound *s, int vol, int pan, int64_t at, int waits) {
    if (!snd.running || !s || !s->data || !s->len || vol <= 0) return;
    pthread_mutex_lock(&lock);
    int pick = -1;
    unsigned best = 0xffffffffu;
    for (int c = 0; c < CHANNELS; ++c) { /* a free channel, else the one nearest its end */
        if (!chan[c].data) {
            pick = c;
            break;
        }
        if (chan[c].waits) continue;
        unsigned left = chan[c].len - chan[c].pos;
        if (left < best) best = left, pick = c;
    }
    if (pick >= 0) {
        vol = clampi(vol, 0, 256), pan = clampi(pan, -256, 256);
        chan[pick].data = s->data, chan[pick].len = s->len, chan[pick].pos = 0, chan[pick].at = at, chan[pick].waits = waits;
        chan[pick].left = vol * (pan > 0 ? 256 - pan : 256) >> 8;
        chan[pick].right = vol * (pan < 0 ? 256 + pan : 256) >> 8;
    }
    pthread_mutex_unlock(&lock);
}
void snd_play(const Sound *s, int vol, int pan) { start(s, vol, pan, 0, 0); }
void snd_play_at(const Sound *s, int vol, int pan, int64_t frame) { start(s, vol, pan, frame, 1); }
void snd_sfx_volume(int v) { snd.sfx_vol = clampi(v, 0, 256); }
void snd_music_volume(int v) { snd.music_vol = clampi(v, 0, 256); }

void snd_music(int mode, int64_t lead) {
    pthread_mutex_lock(&lock);
    snd.mode = mode, snd.paused = 0, snd.ended = 0, snd.muffle = 0;
    snd.pos = snd.from = -lead;
    for (int c = 0; c < CHANNELS; ++c)
        if (chan[c].waits) chan[c].data = 0; /* nothing waits on the music that was */
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&clock_lock);
    snd.heard = (double)-lead, snd.stamp = now_us(), snd.moving = 0, snd.nhist = 0;
    pthread_mutex_unlock(&clock_lock);
}
void snd_pause(int p) { snd.paused = p; }
int snd_music_ended(void) { return snd.ended; }
void snd_muffle(void) { snd.muffle = 0.9f; }
