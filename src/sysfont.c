/* Characters the game's own fonts lack (Japanese, Chinese, Korean, and anything past Latin, Greek
 * and Cyrillic), drawn from the Q2's own font: stock's default.ttf, Arial with a CJK face, the
 * font every Q2 screen draws such names with (Q2 Pod docs/internals.md, Scripts). It is mapped,
 * not read, so its megabytes stay in the page cache, and each glyph is rasterised by
 * stb_truetype (Sean Barrett, public domain; the Q2's own AWTK uses it too) the first time it
 * is drawn at a size, then kept. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "hero.h"

#define SYSFONT "/release/assets/default/raw/fonts/default.ttf"

/* stb_truetype without libm: the few maths calls it makes, ours */
static double s_floor(double x) {
    double i = (double)(long long)x;
    return i > x ? i - 1 : i;
}
static double s_ceil(double x) {
    double i = (double)(long long)x;
    return i < x ? i + 1 : i;
}
static double s_fabs(double x) { return x < 0 ? -x : x; }
static double s_fmod(double x, double y) { return y ? x - y * (double)(long long)(x / y) : 0; }
static double s_pow(double x, double y) { return x <= 0 ? 0 : fexp((float)(y * flog((float)x))); }
static double s_cos(double x) { return fcos((float)x); }
static double s_acos(double x) { /* only for signed distance fields, which the game doesn't make */
    x = x < -1 ? -1 : x > 1 ? 1 : x;
    return PI / 2 - x - x * x * x / 6;
}
#define STBTT_ifloor(x) ((int)s_floor(x))
#define STBTT_iceil(x) ((int)s_ceil(x))
#define STBTT_sqrt(x) __builtin_sqrt(x)
#define STBTT_pow(x, y) s_pow(x, y)
#define STBTT_fmod(x, y) s_fmod(x, y)
#define STBTT_cos(x) s_cos(x)
#define STBTT_acos(x) s_acos(x)
#define STBTT_fabs(x) s_fabs(x)
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u) ((void)(u), free(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wcast-qual"
#include "third_party/stb_truetype.h"
#pragma clang diagnostic pop

static struct {
    int tried, ok;
    unsigned char *data;
    long size;
    stbtt_fontinfo info;
} sf;

#define SLOTS 2048
static struct {
    unsigned cp;
    int size; /* the font's px; 0 empty */
    const FontData *f;
    Glyph g;
    unsigned char *px;
} cache[SLOTS];

static int sysfont_open(void) {
    if (sf.tried) return sf.ok;
    sf.tried = 1;
    const char *path = getenv("HERO_SYSFONT") ? getenv("HERO_SYSFONT") : SYSFONT;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        logf_("sysfont: no %s", path);
        return 0;
    }
    sf.size = (long)lseek(fd, 0, SEEK_END);
    void *p = sf.size > 0 ? mmap(0, (size_t)sf.size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    if (p == MAP_FAILED) return 0;
    sf.data = p;
    int off = stbtt_GetFontOffsetForIndex(sf.data, 0);
    sf.ok = off >= 0 && stbtt_InitFont(&sf.info, sf.data, off);
    logf_("sysfont: %s, %ld bytes%s", path, sf.size, sf.ok ? "" : ", not a font it can read");
    return sf.ok;
}

/* cp from the Q2's font at f's size, placed on f's line; 0 if it has none */
const Glyph *sysfont_glyph(const FontData *f, unsigned cp, const unsigned char **px) {
    unsigned slot = (cp * 2654435761u ^ (unsigned)f->size * 40503u) % SLOTS;
    if (cache[slot].size == f->size && cache[slot].cp == cp && cache[slot].f == f) {
        *px = cache[slot].px;
        return cache[slot].g.adv ? &cache[slot].g : 0; /* a character it has, or none */
    }
    if (!sysfont_open()) return 0;
    int index = stbtt_FindGlyphIndex(&sf.info, (int)cp);
    free(cache[slot].px);
    cache[slot].px = 0, cache[slot].cp = cp, cache[slot].size = f->size, cache[slot].f = f;
    memset(&cache[slot].g, 0, sizeof cache[slot].g);
    if (!index) return *px = 0, (const Glyph *)0;
    float scale = stbtt_ScaleForMappingEmToPixels(&sf.info, (float)f->size);
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&sf.info, index, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&sf.info, index, scale, scale, &x0, &y0, &x1, &y1);
    int w = clampi(x1 - x0, 0, 255), h = clampi(y1 - y0, 0, 255);
    Glyph *g = &cache[slot].g;
    g->cp = (uint16_t)cp, g->w = (uint8_t)w, g->h = (uint8_t)h;
    g->bx = (int8_t)clampi(x0, -127, 127), g->by = (int8_t)clampi(f->ascent + y0, -127, 127);
    g->adv = (uint8_t)clampi((int)(adv * scale + 0.5f), 1, 255);
    if (w && h && (cache[slot].px = malloc((size_t)(w * h))))
        stbtt_MakeGlyphBitmap(&sf.info, cache[slot].px, w, h, w, scale, scale, index);
    else
        g->w = g->h = 0;
    *px = cache[slot].px;
    return g;
}
