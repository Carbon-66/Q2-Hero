/* Q2 Hero's platform layer: the Shanling Q2 under Q2 Pod, as a card game.
 *
 * Q2 Pod (github.com/DiamondBond/q2-pod, docs/internals.md "Card games") forks this binary from
 * Library -> Games when /mnt/mmc/.hero/q2app holds game=1. While it runs, demo stops painting
 * and the game owns the screen and input:
 *
 *   screen  /dev/fb0: the portrait panel, 320 x 375, 32 bpp, red at bit 16, two pages. The UI's
 *           view is landscape, 375 x 320, the panel turned anticlockwise, so a view pixel (X, Y)
 *           is the panel's row X, column 319 - Y. Frames go to the hidden page, then
 *           FBIOPAN_DISPLAY, as q2video, demo and the other card games do.
 *   input   text datagrams on /tmp/q2-hero.input.sock: "k d CODE" / "k u CODE", "t d|m|u X Y"
 *           (the view's px), "w N" (the wheel's travel, 200 a turn, rising anticlockwise) and
 *           "v LEVEL [GAIN]" (the volume, answering /tmp/q2-hero.volume). A thread reads them
 *           and stamps each with the time it came, so a press is judged when it was made, not
 *           at the next frame. Return held 700 ms is Q2 Pod's: it TERMs the game.
 *   sound   audio=1: Q2 Pod stops the music and passes the output as argv[1] (and off the jack the
 *           volume as argv[2]); sound.c plays to it. Play/Pause and Prev/Next come here.
 *
 * For the host tests, HERO_MUSIC and HERO_FFMPEG stand in for /mnt/mmc and /usr/bin/ffmpeg. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "hero.h"

#define APP_DIR "/mnt/mmc/.hero"
#define INPUT_SOCK "/tmp/q2-hero.input.sock"
#define VOLUME_FILE "/tmp/q2-hero.volume"
#define CAPS_FILE "/tmp/q2-addon.caps"
#define LOG_FILE "hero.log"
#define FB_W 320 /* the portrait panel */
#define FB_H 375
#define MIN_AVAIL_MB 12

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY 0x4606

uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)(t.tv_nsec / 1000);
}

void sleep_us(unsigned us) { usleep(us); }

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static char dir[256], root[256], ffmpeg[256], output[32];
static int can_volume;
const char *app_dir(void) { return dir; }
const char *music_root(void) { return root; }
const char *ffmpeg_path(void) { return ffmpeg; }
const char *output_name(void) { return output; }
int volume_can(void) { return can_volume; }

static volatile sig_atomic_t quit_flag;
int quit_asked(void) { return quit_flag; }
static void on_term(int sig) {
    (void)sig;
    quit_flag = 1;
}

/* ---------------------------------------------------------------------------------------------
 * Screen */

static struct {
    int fd;
    unsigned var[40], fix[17]; /* fb_var_screeninfo, fb_fix_screeninfo */
    unsigned char *mem;
    unsigned line;     /* bytes a panel row */
    unsigned y0, back; /* demo's page when we started, the page we draw next */
    int open;
} fbd = { .fd = -1 };

static int fb_open(void) {
    fbd.fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fbd.fd < 0 || ioctl(fbd.fd, FBIOGET_VSCREENINFO, fbd.var) || ioctl(fbd.fd, FBIOGET_FSCREENINFO, fbd.fix))
        return 0;
    fbd.line = fbd.fix[11];
    /* the Q2's panel, or nothing: xres, yres, yres_virtual, bpp, red's offset */
    if (fbd.var[0] != FB_W || fbd.var[1] != FB_H || fbd.var[3] < 2 * FB_H || fbd.var[6] != 32 ||
        fbd.var[8] != 16 || fbd.line < FB_W * 4 || 2 * FB_H * fbd.line > fbd.fix[5])
        return 0;
    fbd.mem = mmap(0, fbd.fix[5], PROT_READ | PROT_WRITE, MAP_SHARED, fbd.fd, 0);
    if (fbd.mem == MAP_FAILED) return 0;
    fbd.y0 = fbd.var[5];
    fbd.back = fbd.y0 ? 0 : FB_H;
    fbd.open = 1;
    return 1;
}

/* view (X, Y) is panel row X, column 319 - Y: each panel row is one view column, written in order */
void present(const uint32_t *frame) {
    if (!fbd.open) return;
    unsigned char *page = fbd.mem + fbd.back * fbd.line;
    for (int x = 0; x < VIEW_W; ++x) {
        uint32_t *out = (uint32_t *)(page + (unsigned)x * fbd.line) + (FB_W - 1);
        const uint32_t *in = frame + x;
        for (int y = 0; y < VIEW_H; ++y, in += VIEW_W) out[-y] = *in | 0xff000000u;
    }
    fbd.var[5] = fbd.back;
    ioctl(fbd.fd, FBIOPAN_DISPLAY, fbd.var);
    fbd.back = fbd.back ? 0 : FB_H;
}

static void fb_close(void) {
    if (!fbd.open) return;
    if (fbd.var[5] != fbd.y0) fbd.var[5] = fbd.y0, ioctl(fbd.fd, FBIOPAN_DISPLAY, fbd.var);
    fbd.open = 0; /* demo repaints the whole screen once we have gone */
}

/* ---------------------------------------------------------------------------------------------
 * Messages: a 5 x 7 font, for errors before anything else is up. */

static const char glyph_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:;!?'-/()_\"=+%";
static const unsigned char glyphs[][7] = {
    { 14, 17, 17, 31, 17, 17, 17 }, { 30, 17, 17, 30, 17, 17, 30 }, { 14, 17, 16, 16, 16, 17, 14 },
    { 30, 17, 17, 17, 17, 17, 30 }, { 31, 16, 16, 30, 16, 16, 31 }, { 31, 16, 16, 30, 16, 16, 16 },
    { 14, 17, 16, 23, 17, 17, 15 }, { 17, 17, 17, 31, 17, 17, 17 }, { 14, 4, 4, 4, 4, 4, 14 },
    { 7, 2, 2, 2, 2, 18, 12 },      { 17, 18, 20, 24, 20, 18, 17 }, { 16, 16, 16, 16, 16, 16, 31 },
    { 17, 27, 21, 21, 17, 17, 17 }, { 17, 17, 25, 21, 19, 17, 17 }, { 14, 17, 17, 17, 17, 17, 14 },
    { 30, 17, 17, 30, 16, 16, 16 }, { 14, 17, 17, 17, 21, 18, 13 }, { 30, 17, 17, 30, 20, 18, 17 },
    { 15, 16, 16, 14, 1, 1, 30 },   { 31, 4, 4, 4, 4, 4, 4 },       { 17, 17, 17, 17, 17, 17, 14 },
    { 17, 17, 17, 17, 17, 10, 4 },  { 17, 17, 17, 21, 21, 21, 10 }, { 17, 17, 10, 4, 10, 17, 17 },
    { 17, 17, 10, 4, 4, 4, 4 },     { 31, 1, 2, 4, 8, 16, 31 },     { 14, 17, 19, 21, 25, 17, 14 },
    { 4, 12, 4, 4, 4, 4, 14 },      { 14, 17, 1, 2, 4, 8, 31 },     { 31, 2, 4, 2, 1, 17, 14 },
    { 2, 6, 10, 18, 31, 2, 2 },     { 31, 16, 30, 1, 1, 17, 14 },   { 6, 8, 16, 30, 17, 17, 14 },
    { 31, 1, 2, 4, 8, 8, 8 },       { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 },
    { 0, 0, 0, 0, 0, 12, 12 },      { 0, 0, 0, 0, 12, 4, 8 },       { 0, 12, 12, 0, 12, 12, 0 },
    { 0, 12, 12, 0, 12, 4, 8 },     { 4, 4, 4, 4, 4, 0, 4 },        { 14, 17, 1, 2, 4, 0, 4 },
    { 4, 4, 8, 0, 0, 0, 0 },        { 0, 0, 0, 31, 0, 0, 0 },       { 1, 1, 2, 4, 8, 16, 16 },
    { 2, 4, 8, 8, 8, 4, 2 },        { 8, 4, 2, 2, 2, 4, 8 },        { 0, 0, 0, 0, 0, 0, 31 },
    { 10, 10, 0, 0, 0, 0, 0 },      { 0, 0, 31, 0, 31, 0, 0 },      { 0, 4, 4, 31, 4, 4, 0 },
    { 24, 25, 2, 4, 8, 19, 3 },
};

static void draw_char(unsigned *dst, int x, int y, char ch, unsigned colour) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    const char *at = strchr(glyph_chars, ch);
    if (!at || !ch) return;
    const unsigned char *g = glyphs[at - glyph_chars];
    for (int r = 0; r < 7; ++r)
        for (int c = 0; c < 5; ++c)
            if (g[r] >> (4 - c) & 1 && x + c >= 0 && x + c < VIEW_W && y + r >= 0 && y + r < VIEW_H)
                dst[(y + r) * VIEW_W + x + c] = colour;
}

void message(const char *title, const char *body) {
    static uint32_t frame[VIEW_W * VIEW_H];
    enum { X0 = 40, COLS = (VIEW_W - 2 * X0) / 6, LINE = 10 };
    unsigned *dst = (unsigned *)frame;
    memset(dst, 0, sizeof frame);
    int y = 40;
    for (int i = 0; title[i] && i < COLS; ++i) draw_char(dst, X0 + 6 * i, y, title[i], 0xffffb030u);
    y += 2 * LINE;
    const char *p = body;
    while (*p && y < VIEW_H - 40) {
        while (*p == ' ') ++p;
        int n = 0, cut = -1;
        while (p[n] && p[n] != '\n' && n < COLS) {
            if (p[n] == ' ') cut = n;
            ++n;
        }
        if (p[n] && p[n] != '\n' && cut > 0) n = cut;
        for (int i = 0; i < n; ++i) draw_char(dst, X0 + 6 * i, y, p[i], 0xffd0d0d8u);
        p += n;
        if (*p == '\n') ++p;
        y += LINE;
    }
    present(frame);
    present(frame); /* both pages */
}

/* ---------------------------------------------------------------------------------------------
 * Input: a thread stamps each datagram and queues it */

static int sock = -1;
static struct {
    char m[48];
    uint64_t t;
} queue[256];
static volatile unsigned q_head, q_tail; /* written by the reader, by the game */
static pthread_t reader;
static int reader_on;
static volatile int reader_stop;

static void *read_input(void *unused) {
    (void)unused;
    char m[64];
    while (!reader_stop) {
        struct pollfd p = { sock, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) continue;
        ssize_t n = recv(sock, m, sizeof m - 1, MSG_DONTWAIT);
        if (n <= 0) continue;
        m[n] = 0;
        unsigned h = q_head;
        if (h - q_tail >= 256) continue; /* full: the game has stalled, drop */
        memcpy(queue[h % 256].m, m, sizeof queue[0].m);
        queue[h % 256].m[sizeof queue[0].m - 1] = 0;
        queue[h % 256].t = now_us();
        __sync_synchronize();
        q_head = h + 1;
    }
    return 0;
}

static uint64_t last_event_t;
uint64_t event_time(void) { return last_event_t; }

int next_event(int *kind, int *a, int *b, int *c) {
    while (q_tail != q_head) {
        __sync_synchronize();
        unsigned t = q_tail;
        char m[48];
        memcpy(m, queue[t % 256].m, sizeof m);
        last_event_t = queue[t % 256].t;
        q_tail = t + 1;
        char ch = 0;
        int x = 0, y = 0;
        if (sscanf(m, "k %c %d", &ch, &x) == 2 && (ch == 'd' || ch == 'u')) {
            *kind = 'k', *a = ch == 'd', *b = x, *c = 0;
            return 1;
        }
        if (sscanf(m, "t %c %d %d", &ch, &x, &y) == 3 && (ch == 'd' || ch == 'm' || ch == 'u')) {
            *kind = 't', *a = ch, *b = x, *c = y;
            return 1;
        }
        if (sscanf(m, "w %d", &x) == 1) {
            *kind = 'w', *a = x, *b = *c = 0;
            return 1;
        }
        if (m[0] == 'v' && m[1] == ' ') {
            char gain[16] = "";
            if (sscanf(m, "v %d %15s", &x, gain) >= 1) {
                *kind = 'v', *a = x, *b = *c = 0;
                if (gain[0]) snd_gain_arg(gain);
                return 1;
            }
        }
    }
    return 0;
}

static int input_open(void) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, INPUT_SOCK);
    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    unlink(INPUT_SOCK);
    if (sock < 0 || bind(sock, (const struct sockaddr *)&addr, sizeof addr)) return 0;
    reader_on = !pthread_create(&reader, 0, read_input, 0);
    return 1;
}

/* ---------------------------------------------------------------------------------------------
 * The Q2's volume: /tmp/q2-hero.volume, written whole by rename, answered with "v LEVEL" */

static void volume_write(const char *text) {
    if (!can_volume) return;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.new", VOLUME_FILE);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    if (n > 0) rename(tmp, VOLUME_FILE);
    else unlink(tmp);
}
void volume_ask(void) { volume_write("?"); }
void volume_set(int level) {
    char t[8];
    snprintf(t, sizeof t, "%d", clampi(level, 0, 100));
    volume_write(t);
}

/* "volume" among the firmware's capability words */
static int caps_has(const char *word) {
    char b[1024];
    int fd = open(getenv("HERO_CAPS") ? getenv("HERO_CAPS") : CAPS_FILE, O_RDONLY | O_CLOEXEC);
    int n = fd >= 0 ? (int)read(fd, b, sizeof b - 1) : 0;
    if (fd >= 0) close(fd);
    b[n > 0 ? n : 0] = 0;
    size_t len = strlen(word);
    for (char *p = b; (p = strstr(p, word)); p += len) {
        int start = p == b || p[-1] == ' ' || p[-1] == '\n' || p[-1] == ',';
        char e = p[len];
        if (start && (e == 0 || e == ' ' || e == '\n' || e == ',' || e == '\r')) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Start and end */

static void cleanup(void) {
    dec_stream_stop();
    snd_shutdown();
    fb_close();
    if (reader_on) reader_stop = 1, pthread_join(reader, 0), reader_on = 0;
    if (sock >= 0) close(sock), sock = -1, unlink(INPUT_SOCK);
}

/* /proc/meminfo's MemAvailable in KB; -1 unknown */
static long mem_available_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[96];
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld", &kb) == 1) break;
    if (f) fclose(f);
    return kb;
}

int main(int argc, char **argv) {
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, on_term);
    signal(SIGPIPE, SIG_IGN);

    snprintf(dir, sizeof dir, "%s", argv[0]);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, "%s", APP_DIR);
    chdir(dir);
    if (freopen(LOG_FILE, "w", stdout)) setvbuf(stdout, 0, _IONBF, 0);
    if (freopen(LOG_FILE, "a", stderr)) setvbuf(stderr, 0, _IONBF, 0);
    snprintf(root, sizeof root, "%s", getenv("HERO_MUSIC") ? getenv("HERO_MUSIC") : "/mnt/mmc");
    snprintf(ffmpeg, sizeof ffmpeg, "%s", getenv("HERO_FFMPEG") ? getenv("HERO_FFMPEG") : "/usr/bin/ffmpeg");
    snprintf(output, sizeof output, "%s", argc > 1 ? argv[1] : "");
    can_volume = caps_has("volume");
    printf("q2-hero %s: started in %s, sound %s %s, music in %s, volume %s\n", VERSION, dir,
           argc > 1 ? argv[1] : "-", argc > 2 ? argv[2] : "-", root, can_volume ? "yes" : "no");

    if (!fb_open()) {
        fprintf(stderr, "No Q2 framebuffer: fd %d, %u x %u, virtual %u, %u bpp, red at %u, line %u, "
                        "%u bytes (errno %d)\n", fbd.fd, fbd.var[0], fbd.var[1], fbd.var[3], fbd.var[6],
                fbd.var[8], fbd.fix[11], fbd.fix[5], errno);
        return 1;
    }
    if (!input_open()) fprintf(stderr, "No input socket at %s (errno %d)\n", INPUT_SOCK, errno);
    /* If memory runs out, the kernel should end the game, never Q2 Pod (demo) */
    FILE *oom = fopen("/proc/self/oom_score_adj", "w");
    if (oom) fputs("1000", oom), fclose(oom);
    long avail = mem_available_kb();
    printf("MemAvailable: %ld KB\n", avail);
    if (avail >= 0 && avail < MIN_AVAIL_MB * 1024) {
        char body[320];
        snprintf(body, sizeof body, "Q2 Hero needs about %d MB of free memory and only %ld MB is free now.\n\n"
                 "Stop other card apps (Streaming) and try again.", MIN_AVAIL_MB, avail / 1024);
        message("NOT ENOUGH MEMORY", body);
        for (int i = 0; i < 600 && !quit_flag; ++i) usleep(50000);
        cleanup();
        return 0;
    }
    if (access(ffmpeg, X_OK)) {
        message("NO FFMPEG", "Q2 Hero plays your songs through the Q2's own ffmpeg, and it isn't at "
                             "/usr/bin/ffmpeg. This Q2 Pod firmware can't run it.");
        for (int i = 0; i < 600 && !quit_flag; ++i) usleep(50000);
        cleanup();
        return 0;
    }
    uint32_t *black = calloc(VIEW_W * VIEW_H, 4); /* both pages black until the game's picture */
    if (black) present(black), present(black), free(black);
    snd_start(argc > 1 ? argv[1] : 0, argc > 2 ? argv[2] : 0);

    uint64_t t0 = now_us();
    int status = game_main();
    printf("Ended after %llu s\n", (unsigned long long)((now_us() - t0) / 1000000));
    cleanup();
    return status;
}
