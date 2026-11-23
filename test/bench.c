/* The cost of the play screen's drawing, alone: bench SONG draws 600 frames of a real chart at
 * Hard, for callgrind or a stopwatch. Links everything but q2.c's main, with stand-ins. */
#define main hero_main_unused
#include "../src/game.c"
#undef main
#include <stdarg.h>
uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000; }
void sleep_us(unsigned us) { usleep(us); }
void present(const uint32_t *f) { (void)f; }
int quit_asked(void) { return 0; }
int next_event(int *k, int *a, int *b, int *c) { (void)k, (void)a, (void)b, (void)c; return 0; }
uint64_t event_time(void) { return 0; }
void logf_(const char *fmt, ...) { (void)fmt; }
void message(const char *t, const char *b) { (void)t, (void)b; }
const char *app_dir(void) { return "."; }
const char *music_root(void) { return "."; }
const char *ffmpeg_path(void) { return getenv("HERO_FFMPEG"); }
const char *output_name(void) { return "plughw:0,0"; }
int volume_can(void) { return 0; }
void volume_ask(void) {}
void volume_set(int l) { (void)l; }
int main(int argc, char **argv) {
    Listener *l = listen_new();
    SongInfo info;
    dec_listen(argv[1], &info, sink, l, 0, 0);
    listen_finish(l, &an);
    chart_make(&an, HARD, &chart);
    memset(&g, 0, sizeof g);
    for (int i = 0; i < LANES; ++i) g.held[i] = -1;
    g.rock = 0.7f, g.hero = 0.6f;
    uint64_t t0 = now_us();
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    for (int f = 0; f < frames; ++f) {
        g.now = 2 + f / 60.0f;
        if (f % 20 == 0) spawn(lane_x(f % 3, 0), STRIKE_Y, lane_col[f % 3], 10, 0), spawn(lane_x(f % 3, 0), STRIKE_Y, lane_col[f % 3], 5, 1);
        draw_play(1 / 60.0f);
    }
    printf("%.3f ms a frame\n", (now_us() - t0) / 1000.0 / frames);
    return 0;
}
