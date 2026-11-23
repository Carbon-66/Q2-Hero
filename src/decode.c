/* Songs through the Q2's own ffmpeg (/usr/bin/ffmpeg, which Q2 Pod's q2video and Groovebox use
 * too), run with no shell, since card names hold quotes and $:
 *
 *   listen   the whole song, stereo at 11025 Hz, for analyze.c; ffmpeg's report on stderr gives the
 *            length (for the progress bar), the title and the artist.
 *   stream   the song as played, stereo at 44100 Hz, read by a thread into sound.c's ring while
 *            the game runs. The two decodes are the same decoder on the same file, so a note
 *            charted from the first lands on the same frame of the second. */
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hero.h"

#define F_SETFD 2
#define FD_CLOEXEC 1
#define STALL_MS 20000 /* no output for this long and ffmpeg is given up on */

/* ffmpeg with args; its stdout onto *out (and stderr onto *err, unless err is 0) */
static pid_t spawn(const char **args, int *out, int *err) {
    int o[2], e[2] = { -1, -1 };
    if (pipe(o)) return -1;
    if (err && pipe(e)) return close(o[0]), close(o[1]), -1;
    fcntl(o[0], F_SETFD, FD_CLOEXEC);
    if (err) fcntl(e[0], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close(o[0]), close(o[1]);
        if (err) close(e[0]), close(e[1]);
        return -1;
    }
    if (!pid) { /* only async-signal-safe calls until the exec */
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0), dup2(o[1], 1);
        if (err) dup2(e[1], 2);
        if (o[1] > 2) close(o[1]);
        if (err && e[1] > 2) close(e[1]);
        execv(ffmpeg_path(), (char *const *)args);
        _exit(127);
    }
    close(o[1]);
    if (err) close(e[1]);
    *out = o[0];
    if (err) *err = e[0];
    return pid;
}

static void reap(pid_t pid) {
    kill(pid, SIGKILL); /* done, stalled or cancelled: either way it is over */
    waitpid(pid, 0, 0);
}

/* "    title           : Song" -> Song, when the key is key (any case) */
static int meta(const char *line, const char *key, char *out, int n) {
    while (*line == ' ') ++line;
    size_t k = strlen(key);
    if (strncasecmp(line, key, k)) return 0;
    const char *p = line + k;
    while (*p == ' ') ++p;
    if (*p != ':') return 0;
    ++p;
    if (*p == ' ') ++p;
    snprintf(out, (size_t)n, "%s", p);
    return out[0] != 0;
}

static void report_line(const char *line, SongInfo *info, int *done) {
    int h, m;
    float s;
    const char *d = strstr(line, "Duration: ");
    if (d && sscanf(d + 10, "%d:%d:%f", &h, &m, &s) == 3) info->duration = h * 3600.0 + m * 60.0 + s;
    if (strstr(line, "Output #0") || strstr(line, "Stream mapping")) *done = 1;
    if (*done) return;
    if (!info->title[0]) meta(line, "title", info->title, sizeof info->title);
    if (!info->artist[0]) meta(line, "artist", info->artist, sizeof info->artist);
    if (!info->artist[0]) meta(line, "album_artist", info->artist, sizeof info->artist);
}

int dec_listen(const char *file, SongInfo *info, void (*sink)(const int16_t *, unsigned, void *), void *ctx,
               volatile int *cancel, volatile float *progress) {
    char rate[16];
    snprintf(rate, sizeof rate, "%d", AN_RATE);
    const char *args[] = { ffmpeg_path(), "-nostdin", "-hide_banner", "-nostats", "-i", file, "-map", "0:a:0",
                           "-f", "s16le", "-ac", "2", "-ar", rate, "pipe:1", 0 };
    int out, err;
    memset(info, 0, sizeof *info);
    pid_t pid = spawn(args, &out, &err);
    if (pid < 0) return -1;
    static int16_t buf[8192];
    char line[512];
    int ll = 0, done = 0, rem = 0, open_err = 1, idle = 0;
    unsigned long long got = 0;
    unsigned char carry[4];
    for (;;) {
        if (cancel && *cancel) break;
        struct pollfd p[2] = { { out, POLLIN, 0 }, { err, POLLIN, 0 } };
        int r = poll(p, open_err ? 2 : 1, 200);
        if (r < 0) break;
        if (!r) {
            if (++idle > STALL_MS / 200) break;
            continue;
        }
        idle = 0;
        if (open_err && p[1].revents) {
            char c[256];
            ssize_t n = read(err, c, sizeof c);
            if (n <= 0) open_err = 0;
            for (ssize_t i = 0; i < n; ++i) {
                if (c[i] == '\n' || c[i] == '\r') {
                    line[ll] = 0;
                    if (ll) report_line(line, info, &done);
                    ll = 0;
                } else if (ll < (int)sizeof line - 1)
                    line[ll++] = c[i];
            }
        }
        if (p[0].revents) {
            unsigned char *b = (unsigned char *)buf; /* stereo frames, 4 bytes; a part frame waits */
            memcpy(b, carry, (size_t)rem);
            ssize_t n = read(out, b + rem, sizeof buf - (size_t)rem);
            if (n <= 0) break;
            n += rem;
            rem = (int)(n & 3);
            memcpy(carry, b + n - rem, (size_t)rem);
            unsigned frames = (unsigned)(n / 4);
            sink(buf, frames, ctx);
            got += frames;
            if (progress && info->duration > 0) *progress = clampf((float)(got / (info->duration * AN_RATE)), 0, 1);
        }
    }
    int cancelled = cancel && *cancel;
    close(out), close(err);
    reap(pid);
    if (!info->duration) info->duration = (double)got / AN_RATE;
    logf_("listen: %s: %llu frames (%.1f s by the report), \"%s\" by \"%s\"%s", file, got, info->duration,
          info->title, info->artist, cancelled ? ", cancelled" : "");
    if (cancelled) return 1;
    return got > AN_RATE ? 0 : -1;
}

/* ---------------------------------------------------------------------------------------------
 * The stream */
static struct {
    pthread_t thread;
    int on, fd;
    pid_t pid;
    volatile int stop;
} st = { .fd = -1 };

static void *streamer(void *unused) {
    (void)unused;
    static int16_t buf[2 * 2048];
    unsigned have = 0; /* bytes in buf */
    for (;;) {
        if (st.stop) break;
        unsigned frames = have / 4;
        if (frames) {
            unsigned space = ring_space(), n = frames < space ? frames : space;
            if (n) {
                ring_put(buf, n);
                memmove(buf, (char *)buf + n * 4, have - n * 4);
                have -= n * 4;
            } else {
                usleep(5000); /* the ring is full: the song is well ahead */
                continue;
            }
        }
        struct pollfd p = { st.fd, POLLIN, 0 };
        int r = poll(&p, 1, 100);
        if (r <= 0) continue;
        ssize_t n = read(st.fd, (char *)buf + have, sizeof buf - have);
        if (n <= 0) {
            while (!st.stop && have >= 4) { /* the last of it */
                unsigned space = ring_space(), k = have / 4 < space ? have / 4 : space;
                if (!k) {
                    usleep(5000);
                    continue;
                }
                ring_put(buf, k);
                memmove(buf, (char *)buf + k * 4, have - k * 4);
                have -= k * 4;
            }
            ring_eof(1);
            break;
        }
        have += (unsigned)n;
    }
    return 0;
}

int dec_stream_start(const char *file) {
    dec_stream_stop();
    char rate[16];
    snprintf(rate, sizeof rate, "%d", RATE);
    const char *args[] = { ffmpeg_path(), "-nostdin", "-hide_banner", "-nostats", "-v", "error", "-i", file,
                           "-map", "0:a:0", "-f", "s16le", "-ac", "2", "-ar", rate, "pipe:1", 0 };
    ring_reset();
    st.pid = spawn(args, &st.fd, 0);
    if (st.pid < 0) return 0;
    st.stop = 0;
    st.on = !pthread_create(&st.thread, 0, streamer, 0);
    if (!st.on) close(st.fd), st.fd = -1, reap(st.pid);
    return st.on;
}

void dec_stream_stop(void) {
    if (!st.on) return;
    st.stop = 1;
    pthread_join(st.thread, 0);
    close(st.fd), st.fd = -1;
    reap(st.pid);
    st.on = 0;
    ring_reset();
}
