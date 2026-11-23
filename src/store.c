/* What Q2 Hero keeps in its card folder:
 *
 *   settings.txt    the options, "key value" a line
 *   scores.txt      the best on each song and difficulty: "song diff score stars pct streak"
 *   charts/<h>.bin  what analyze.c heard in a song, so it is listened to once
 *
 * A song is known by the FNV-1a hash of its path on the card. Each file is written whole, then
 * renamed over the old, so a game ended mid-write loses nothing. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hero.h"

Settings set = { .speed = 3, .sfx = 2, .last_diff = MEDIUM };

void settings_load(void) {
    FILE *f = fopen("settings.txt", "r");
    char line[600];
    while (f && fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        int v;
        if (sscanf(line, "speed %d", &v) == 1) set.speed = clampi(v, 1, 5);
        else if (sscanf(line, "lefty %d", &v) == 1) set.lefty = !!v;
        else if (sscanf(line, "nofail %d", &v) == 1) set.nofail = !!v;
        else if (sscanf(line, "sfx %d", &v) == 1) set.sfx = clampi(v, 0, 2);
        else if (sscanf(line, "offset_jack %d", &v) == 1) set.offset_ms[0] = clampi(v, -300, 600);
        else if (sscanf(line, "offset_bt %d", &v) == 1) set.offset_ms[1] = clampi(v, -300, 600);
        else if (sscanf(line, "offset_usb %d", &v) == 1) set.offset_ms[2] = clampi(v, -300, 600);
        else if (sscanf(line, "diff %d", &v) == 1) set.last_diff = clampi(v, 0, DIFFS - 1);
        else if (!strncmp(line, "dir ", 4)) snprintf(set.last_dir, sizeof set.last_dir, "%s", line + 4);
    }
    if (f) fclose(f);
}

void settings_save(void) {
    FILE *f = fopen("settings.txt.new", "w");
    if (!f) return;
    fprintf(f, "speed %d\nlefty %d\nnofail %d\nsfx %d\noffset_jack %d\noffset_bt %d\noffset_usb %d\ndiff %d\ndir %s\n",
            set.speed, set.lefty, set.nofail, set.sfx, set.offset_ms[0], set.offset_ms[1], set.offset_ms[2],
            set.last_diff, set.last_dir);
    if (!fclose(f)) rename("settings.txt.new", "settings.txt");
}

/* the output this run plays through: the jack, Bluetooth or USB, each with its own lag */
int *offset_for_output(void) {
    const char *o = output_name();
    if (strstr(o, "bluealsa")) return &set.offset_ms[1];
    if (!strcmp(o, "usb")) return &set.offset_ms[2];
    return &set.offset_ms[0];
}

/* ---------------------------------------------------------------------------------------------
 * Best scores, read whole each time: a few hundred lines at most */
typedef struct {
    uint32_t song;
    int diff;
    Best b;
} Row;
static Row *rows;
static int nrows, loaded;

static void scores_load(void) {
    if (loaded) return;
    loaded = 1;
    FILE *f = fopen("scores.txt", "r");
    char line[128];
    int cap = 0;
    while (f && fgets(line, sizeof line, f)) {
        Row r;
        unsigned song;
        if (sscanf(line, "%x %d %d %d %d %d", &song, &r.diff, &r.b.score, &r.b.stars, &r.b.pct, &r.b.streak) != 6) continue;
        r.song = song;
        if (nrows == cap) {
            Row *m = realloc(rows, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *rows);
            if (!m) break;
            rows = m;
        }
        rows[nrows++] = r;
    }
    if (f) fclose(f);
}

static void scores_save(void) {
    FILE *f = fopen("scores.txt.new", "w");
    if (!f) return;
    for (int i = 0; i < nrows; ++i)
        fprintf(f, "%08x %d %d %d %d %d\n", (unsigned)rows[i].song, rows[i].diff, rows[i].b.score, rows[i].b.stars,
                rows[i].b.pct, rows[i].b.streak);
    if (!fclose(f)) rename("scores.txt.new", "scores.txt");
}

Best best_get(uint32_t song, int diff) {
    scores_load();
    for (int i = 0; i < nrows; ++i)
        if (rows[i].song == song && rows[i].diff == diff) return rows[i].b;
    Best none = { 0, 0, 0, 0 };
    return none;
}

int best_any(uint32_t song) {
    scores_load();
    int stars = -1;
    for (int i = 0; i < nrows; ++i)
        if (rows[i].song == song && rows[i].b.stars > stars) stars = rows[i].b.stars;
    return stars;
}

int best_put(uint32_t song, int diff, Best b) {
    scores_load();
    for (int i = 0; i < nrows; ++i)
        if (rows[i].song == song && rows[i].diff == diff) {
            if (b.score <= rows[i].b.score) return 0;
            rows[i].b = b;
            scores_save();
            return 1;
        }
    Row *m = realloc(rows, (size_t)(nrows + 1) * sizeof *rows);
    if (!m) return 0;
    rows = m;
    rows[nrows].song = song, rows[nrows].diff = diff, rows[nrows].b = b;
    ++nrows;
    scores_save();
    return 1;
}

void cache_path(char *out, int n, uint32_t song) {
    mkdir("charts", 0755);
    snprintf(out, (size_t)n, "charts/%08x.bin", (unsigned)song);
}

long file_size(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    long size = (long)lseek(fd, 0, SEEK_END);
    close(fd);
    return size;
}
