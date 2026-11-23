/* alsa-lib for the host build: a PCM that plays at the real rate from a 4096-frame buffer, as a
 * device does, so snd_pcm_delay has something true to say. It keeps the loudest sample, and
 * with $FAKE_PCM_DUMP set, writes everything played there (raw s16le stereo), so a test can
 * line what was heard up with what the game did. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BUF 4096
int fake_pcm_opens, fake_pcm_peak;
long fake_pcm_frames;
static int handle;
static double t0 = -1;
static FILE *dump, *onsets;
static long quiet; /* frames since the last loud one */

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static long played(void) { return t0 < 0 ? 0 : (long)((now() - t0) * 44100); }

int snd_pcm_open(void **pcm, const char *dev, int a, int b) {
    (void)a, (void)b;
    if (strcmp(dev, "plughw:0,0") && strncmp(dev, "plughw:", 7) && strcmp(dev, "plug:bluealsa")) return -2;
    *pcm = &handle;
    ++fake_pcm_opens;
    if (getenv("FAKE_PCM_DUMP")) dump = fopen(getenv("FAKE_PCM_DUMP"), "wb");
    if (getenv("FAKE_PCM_ONSETS")) onsets = fopen(getenv("FAKE_PCM_ONSETS"), "w");
    return 0;
}
int snd_pcm_close(void *p) { (void)p; if (dump) fclose(dump), dump = 0; return 0; }
int snd_pcm_drop(void *p) { (void)p; return 0; }
int snd_pcm_recover(void *p, int e, int s) { (void)p, (void)s; return e; }
int snd_pcm_set_params(void *p, int f, int a, unsigned c, unsigned r, int s, unsigned l) {
    (void)p, (void)f, (void)a, (void)c, (void)r, (void)s, (void)l;
    return 0;
}
int snd_pcm_delay(void *p, long *d) {
    (void)p;
    long q = fake_pcm_frames - played();
    if (q < 0) q = 0;
    *d = q;
    return 0;
}
long snd_pcm_writei(void *p, const void *buf, unsigned long frames) {
    (void)p;
    if (t0 < 0) t0 = now();
    long q = fake_pcm_frames - played();
    if (q < 0) { /* an underrun: the device restarts from here */
        t0 = now() - fake_pcm_frames / 44100.0;
        q = 0;
    }
    if (q + (long)frames > BUF) usleep((unsigned)((q + (long)frames - BUF) * 1000000L / 44100));
    const short *s = buf;
    for (unsigned long i = 0; i < 2 * frames; ++i) {
        int v = s[i] < 0 ? -s[i] : s[i];
        if (v > fake_pcm_peak) fake_pcm_peak = v;
    }
    if (dump) fwrite(buf, 4, frames, dump);
    for (unsigned long f = 0; onsets && f < frames; ++f) { /* a sound after 50 ms of quiet: when it plays */
        int v = s[2 * f] < 0 ? -s[2 * f] : s[2 * f];
        if (v > 1500 && quiet > 2205) fprintf(onsets, "%.6f\n", t0 + (fake_pcm_frames + (long)f) / 44100.0), fflush(onsets);
        quiet = v > 1500 ? 0 : quiet + 1;
    }
    fake_pcm_frames += (long)frames;
    return (long)frames;
}
const char *snd_strerror(int e) { (void)e; return "fake"; }

__attribute__((destructor)) static void report(void) {
    fprintf(stderr, "fake pcm: %d opens, %ld frames, peak %d\n", fake_pcm_opens, fake_pcm_frames, fake_pcm_peak);
}
