/* The listening and charting alone, on the host: listen_tool FILE [DIFF] prints the tempo, the
 * beats, the hits and each difficulty's notes, so test/accuracy.py can check them against the
 * song's truth. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/hero.h"

const char *ffmpeg_path(void) { return getenv("HERO_FFMPEG") ? getenv("HERO_FFMPEG") : "ffmpeg"; }
void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
unsigned ring_space(void) { return 0; }
void ring_put(const int16_t *f, unsigned n) { (void)f, (void)n; }
void ring_reset(void) {}
void ring_eof(int e) { (void)e; }
static void sink(const int16_t *p, unsigned n, void *c) { listen_feed(c, p, n); }

int main(int argc, char **argv) {
    Listener *l = listen_new();
    SongInfo info;
    if (dec_listen(argv[1], &info, sink, l, 0, 0)) return 1;
    Analysis a;
    if (listen_finish(l, &a)) return 2;
    printf("bpm %.2f\nduration %.2f\ntitle %s\nartist %s\n", a.bpm, a.duration, info.title, info.artist);
    for (int i = 0; i < a.nbeats; ++i) printf("beat %.4f\n", a.beat[i]);
    for (int i = 0; i < a.nonsets; ++i) printf("hit %.4f %.3f %d %.2f %.2f %.3f\n", a.on[i].t, a.on[i].strength, a.on[i].kind, a.on[i].pitch, a.on[i].dur, a.on[i].wobble);
    for (int d = 0; d < DIFFS; ++d) {
        Chart c;
        chart_make(&a, d, &c);
        for (int i = 0; i < c.count; ++i) printf("note %d %.4f %d %.3f %d\n", d, c.n[i].t, c.n[i].lanes, c.n[i].len, c.n[i].hero);
        chart_free(&c);
    }
    return 0;
}
