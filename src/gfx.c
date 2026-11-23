/* Drawing into the frame (375 x 320 XRGB, then present()), smoothed at the edges, and the maths
 * the game needs without libm. */
#include <string.h>

#include "hero.h"

static uint32_t frame[VIEW_W * VIEW_H];
uint32_t *fb = frame;
static int cx0, cy0, cx1 = VIEW_W, cy1 = VIEW_H;

void clip(int x0, int y0, int x1, int y1) {
    cx0 = clampi(x0, 0, VIEW_W), cy0 = clampi(y0, 0, VIEW_H), cx1 = clampi(x1, 0, VIEW_W), cy1 = clampi(y1, 0, VIEW_H);
}
void clip_reset(void) { cx0 = 0, cy0 = 0, cx1 = VIEW_W, cy1 = VIEW_H; }


static inline void put(int x, int y, uint32_t c, int alpha) {
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1 || alpha <= 0) return;
    uint32_t *p = &fb[y * VIEW_W + x];
    *p = alpha >= 256 ? c : mix(*p, c, alpha);
}
static inline void add(int x, int y, uint32_t c, int alpha) {
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1 || alpha <= 0) return;
    uint32_t *p = &fb[y * VIEW_W + x];
    int r = (*p >> 16 & 255) + ((int)(c >> 16 & 255) * alpha >> 8);
    int g = (*p >> 8 & 255) + ((int)(c >> 8 & 255) * alpha >> 8);
    int b = (*p & 255) + ((int)(c & 255) * alpha >> 8);
    *p = (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
}

void fill(uint32_t c) {
    for (int i = 0; i < VIEW_W * VIEW_H; ++i) fb[i] = c;
}

void gradient(int y0, int y1, uint32_t top, uint32_t bottom) {
    for (int y = clampi(y0, cy0, cy1); y < clampi(y1, cy0, cy1); ++y) {
        uint32_t c = mix(top, bottom, (y - y0) * 256 / (y1 - y0 > 0 ? y1 - y0 : 1));
        for (int x = cx0; x < cx1; ++x) fb[y * VIEW_W + x] = c;
    }
}

void rect(int x, int y, int w, int h, uint32_t c, int alpha) {
    int x0 = clampi(x, cx0, cx1), x1 = clampi(x + w, cx0, cx1), y0 = clampi(y, cy0, cy1), y1 = clampi(y + h, cy0, cy1);
    for (int j = y0; j < y1; ++j)
        for (int i = x0; i < x1; ++i) put(i, j, c, alpha);
}

void darken(int alpha) { rect(0, 0, VIEW_W, VIEW_H, 0, alpha); }

/* coverage of a pixel at signed distance d from an edge (inside negative) */
static inline int cover(float d) { return d <= -0.5f ? 256 : d >= 0.5f ? 0 : (int)((0.5f - d) * 256); }

static float rr_dist(float px, float py, float x, float y, float w, float h, float r) {
    float qx = fabs_(px - (x + w * 0.5f)) - (w * 0.5f - r), qy = fabs_(py - (y + h * 0.5f)) - (h * 0.5f - r);
    float ox = maxf(qx, 0), oy = maxf(qy, 0);
    return fsqrt(ox * ox + oy * oy) + minf(maxf(qx, qy), 0) - r;
}

void rrect(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    r = clampi(r, 0, (w < h ? w : h) / 2);
    for (int j = y; j < y + h; ++j) {
        if (j < cy0 || j >= cy1) continue;
        int corner = j < y + r || j >= y + h - r;
        for (int i = x; i < x + w; ++i) {
            if (!corner && i >= x + r && i < x + w - r) {
                put(i, j, c, alpha);
                continue;
            }
            int a = cover(rr_dist(i + 0.5f, j + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r));
            put(i, j, c, a * alpha >> 8);
        }
    }
}

void rrect_line(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    for (int j = y - 1; j <= y + h; ++j)
        for (int i = x - 1; i <= x + w; ++i) {
            if (j > y + r + 1 && j < y + h - r - 2 && i > x + 1 && i < x + w - 2) {
                i = x + w - 2;
                continue;
            }
            float d = rr_dist(i + 0.5f, j + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r);
            int a = cover(fabs_(d + 0.75f) - 0.75f);
            put(i, j, c, a * alpha >> 8);
        }
}

/* by rows: the part of a row surely inside is filled straight, and only the pixels near the
 * edge are measured */
void ellipse(float cx, float cy, float rx, float ry, uint32_t c, int alpha) {
    if (rx < 0.3f || ry < 0.3f || alpha <= 0) return;
    int y0 = (int)(cy - ry - 1), y1 = (int)(cy + ry + 2);
    float m = minf(rx, ry), irx = 1 / rx, iry = 1 / ry;
    for (int j = y0; j < y1; ++j) {
        if (j < cy0 || j >= cy1) continue;
        float py = j + 0.5f - cy, dy = py * iry, ay = fabs_(py);
        float din = (ay + 1) * iry, dout = maxf(0, ay - 1) * iry;
        if (dout >= 1) continue;
        float hin = din < 1 ? rx * fsqrt(1 - din * din) - 1.0f : -1, hout = rx * fsqrt(1 - dout * dout) + 1.0f;
        int xa = (int)(cx - hout), xb = (int)(cx + hout) + 1;
        if (xa < cx0) xa = cx0;
        if (xb > cx1) xb = cx1;
        uint32_t *row = fb + j * VIEW_W;
        for (int i = xa; i < xb; ++i) {
            float px = i + 0.5f - cx;
            if (fabs_(px) < hin) {
                row[i] = alpha >= 256 ? c : mix(row[i], c, alpha);
                continue;
            }
            float dx = px * irx, d = fsqrt(dx * dx + dy * dy);
            int a = cover((d - 1) * m) * alpha >> 8;
            if (a > 0) row[i] = a >= 256 ? c : mix(row[i], c, a);
        }
    }
}
void disc(float cx, float cy, float r, uint32_t c, int alpha) { ellipse(cx, cy, r, r, c, alpha); }

void ring(float cx, float cy, float rx, float ry, float thick, uint32_t c, int alpha) {
    if (rx < 0.5f || ry < 0.5f) return;
    int y0 = (int)(cy - ry - 1), y1 = (int)(cy + ry + 2), x0 = (int)(cx - rx - 1), x1 = (int)(cx + rx + 2);
    float m = minf(rx, ry), irx = 1 / rx, iry = 1 / ry, half = thick * 0.5f;
    for (int j = y0; j < y1; ++j) {
        if (j < cy0 || j >= cy1) continue;
        float dy = (j + 0.5f - cy) * iry;
        for (int i = x0; i < x1; ++i) {
            float dx = (i + 0.5f - cx) * irx, d = (fsqrt(dx * dx + dy * dy) - 1) * m + half;
            put(i, j, c, cover(fabs_(d) - half) * alpha >> 8);
        }
    }
}

/* additive, falling off as (1 - d^2/r^2)^2: one shape made once, 257 px across, stepped
 * through at the glow's scale a row at a time */
#define GS 128
static uint8_t gshape[(2 * GS + 1) * (2 * GS + 1)];
void glow(float cx, float cy, float r, uint32_t c, int alpha) {
    if (r < 1 || alpha <= 0) return;
    if (!gshape[GS * (2 * GS + 1) + GS])
        for (int y = -GS; y <= GS; ++y)
            for (int x = -GS; x <= GS; ++x) {
                int d2 = x * x + y * y, q = GS * GS - d2;
                gshape[(y + GS) * (2 * GS + 1) + x + GS] = q > 0 ? (uint8_t)((long long)q * q * 255 / ((long long)GS * GS * GS * GS)) : 0;
            }
    int y0 = (int)(cy - r), y1 = (int)(cy + r + 1);
    int cr = (int)(c >> 16 & 255), cg = (int)(c >> 8 & 255), cb = (int)(c & 255);
    alpha = alpha > 256 ? 256 : alpha;
    int step = (int)(GS * 65536.0f / r); /* shape px a view px, 16.16 */
    for (int j = y0; j < y1; ++j) {
        if (j < cy0 || j >= cy1) continue;
        float dy = j + 0.5f - cy, w2 = r * r - dy * dy;
        if (w2 <= 0) continue;
        float w = fsqrt(w2);
        int xa = (int)(cx - w), xb = (int)(cx + w) + 1;
        if (xa < cx0) xa = cx0;
        if (xb > cx1) xb = cx1;
        int sy = clampi((int)(dy * GS / r) + GS, 0, 2 * GS);
        const uint8_t *shape = gshape + sy * (2 * GS + 1);
        int sx = (int)((xa + 0.5f - cx) * step) + (GS << 16);
        uint32_t *row = fb + j * VIEW_W;
        for (int i = xa; i < xb; ++i, sx += step) {
            int k = sx >> 16;
            if ((unsigned)k > 2 * GS) continue;
            int a = shape[k] * alpha >> 8;
            if (!a) continue;
            uint32_t p = row[i];
            int pr = (int)(p >> 16 & 255) + (cr * a >> 8), pg = (int)(p >> 8 & 255) + (cg * a >> 8), pb = (int)(p & 255) + (cb * a >> 8);
            row[i] = (uint32_t)(pr > 255 ? 255 : pr) << 16 | (uint32_t)(pg > 255 ? 255 : pg) << 8 | (uint32_t)(pb > 255 ? 255 : pb);
        }
    }
}

void line(float x0, float y0, float x1, float y1, float w, uint32_t c, int alpha) {
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, half = w * 0.5f;
    int bx0 = (int)(minf(x0, x1) - half - 1), bx1 = (int)(maxf(x0, x1) + half + 2);
    int by0 = (int)(minf(y0, y1) - half - 1), by1 = (int)(maxf(y0, y1) + half + 2);
    for (int j = by0; j < by1; ++j) {
        if (j < cy0 || j >= cy1) continue;
        for (int i = bx0; i < bx1; ++i) {
            float px = i + 0.5f - x0, py = j + 0.5f - y0;
            float t = len2 > 0 ? clampf((px * dx + py * dy) / len2, 0, 1) : 0;
            float ex = px - t * dx, ey = py - t * dy;
            put(i, j, c, cover(fsqrt(ex * ex + ey * ey) - half) * alpha >> 8);
        }
    }
}

/* a convex polygon, by rows: each row's span from where its edges cross the row's middle, with
 * the span's ends shaded by how much of their pixel they cover */
static void poly(const float *xy, int n, uint32_t c, int alpha) {
    float ymin = 1e9f, ymax = -1e9f;
    for (int k = 0; k < n; ++k) ymin = minf(ymin, xy[2 * k + 1]), ymax = maxf(ymax, xy[2 * k + 1]);
    for (int j = (int)ymin; j <= (int)ymax; ++j) {
        if (j < cy0 || j >= cy1) continue;
        float y = j + 0.5f, lo = 1e9f, hi = -1e9f;
        for (int k = 0; k < n; ++k) {
            float ax = xy[2 * k], ay = xy[2 * k + 1], bx = xy[2 * ((k + 1) % n)], by = xy[2 * ((k + 1) % n) + 1];
            if ((ay <= y && by > y) || (by <= y && ay > y)) {
                float x = ax + (y - ay) / (by - ay) * (bx - ax);
                lo = minf(lo, x), hi = maxf(hi, x);
            }
        }
        if (lo > hi) continue;
        float cov_y = 1;
        if (j + 1 > ymax) cov_y = ymax - j;
        if (j < ymin) cov_y = j + 1 - ymin;
        int ay = (int)(clampf(cov_y, 0, 1) * alpha);
        int i0 = (int)lo, i1 = (int)hi;
        if (i0 == i1) {
            put(i0, j, c, (int)((hi - lo) * ay));
            continue;
        }
        put(i0, j, c, (int)((i0 + 1 - lo) * ay));
        for (int i = i0 + 1; i < i1; ++i) put(i, j, c, ay);
        put(i1, j, c, (int)((hi - i1) * ay));
    }
}
void quad(const float *xy, uint32_t c, int alpha) { poly(xy, 4, c, alpha); }

void star(float cx, float cy, float r, uint32_t c, int alpha) {
    float in = r * 0.42f, pent[10], tri[6];
    for (int k = 0; k < 5; ++k) {
        float a = -PI / 2 + PI / 5 + k * TAU / 5;
        pent[2 * k] = cx + fcos(a) * in, pent[2 * k + 1] = cy + fsin(a) * in;
    }
    poly(pent, 5, c, alpha);
    for (int k = 0; k < 5; ++k) {
        float a = -PI / 2 + k * TAU / 5;
        tri[0] = cx + fcos(a) * r, tri[1] = cy + fsin(a) * r;
        tri[2] = pent[2 * ((k + 4) % 5)], tri[3] = pent[2 * ((k + 4) % 5) + 1];
        tri[4] = pent[2 * k], tri[5] = pent[2 * k + 1];
        poly(tri, 3, c, alpha);
    }
}

/* ---------------------------------------------------------------------------------------------
 * Text: UTF-8 onto the alpha bitmaps of fontdata.c */

static unsigned utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xc0 && c < 0xe0 && (*p & 0xc0) == 0x80) c = (c & 31) << 6 | (*p++ & 63);
    else if (c >= 0xe0 && c < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80)
        c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63), p += 2;
    else if (c >= 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80)
        c = 0xfffd, p += 3;
    else if (c >= 0x80)
        c = 0xfffd;
    *s = (const char *)p;
    return c;
}

/* cp in f; else a near stand-in f has (capitals for a capitals-only face, plain quotes and
 * dashes); else the Q2's own font at f's size (sysfont.c); else ?. px: its alpha pixels */
static const Glyph *glyph_in(const FontData *f, unsigned cp) {
    int lo = 0, hi = f->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f->g[m].cp == cp) return &f->g[m];
        if (f->g[m].cp < cp) lo = m + 1;
        else hi = m - 1;
    }
    return 0;
}
static const Glyph *glyph(const FontData *f, unsigned cp, const unsigned char **px) {
    const Glyph *g = glyph_in(f, cp);
    if (!g && cp >= 'a' && cp <= 'z') g = glyph_in(f, cp - 32);
    if (!g && (cp == 0x2019 || cp == 0x2018)) g = glyph_in(f, '\'');
    if (!g && (cp == 0x201c || cp == 0x201d)) g = glyph_in(f, '"');
    if (!g && (cp == 0x2013 || cp == 0x2014)) g = glyph_in(f, '-');
    if (g) {
        *px = f->px + g->off;
        return g;
    }
    if (cp > 0x7f && cp != 0xfffd && (g = sysfont_glyph(f, cp, px))) return g;
    g = glyph_in(f, '?');
    *px = g ? f->px + g->off : 0;
    return g;
}

int text_w(const FontData *f, const char *s) {
    int w = 0;
    const unsigned char *px;
    while (*s) {
        const Glyph *g = glyph(f, utf8(&s), &px);
        if (g) w += g->adv;
    }
    return w;
}

static void draw_glyph(const Glyph *g, const unsigned char *px, int x, int y, uint32_t c, int alpha, int additive) {
    if (!px) return;
    for (int j = 0; j < g->h; ++j)
        for (int i = 0; i < g->w; ++i) {
            int a = px[j * g->w + i];
            if (!a) continue;
            if (additive) add(x + g->bx + i, y + g->by + j, c, a * alpha >> 8);
            else put(x + g->bx + i, y + g->by + j, c, a * alpha >> 8);
        }
}

/* a glyph from the Q2's regular font in one of the bold faces: drawn twice, a pixel apart, as Q2
 * Pod thickens its own fallback text */
static int thicken(const FontData *f, const Glyph *g) { return f != &font_small && (g < f->g || g >= f->g + f->n); }

void text(const FontData *f, const char *s, int x, int y, uint32_t c, int alpha) {
    const unsigned char *px;
    while (*s) {
        const Glyph *g = glyph(f, utf8(&s), &px);
        if (!g) continue;
        if (g->w) {
            draw_glyph(g, px, x, y, c, alpha, 0);
            if (thicken(f, g)) draw_glyph(g, px, x + 1, y, c, alpha, 0);
        }
        x += g->adv;
    }
}
void text_c(const FontData *f, const char *s, int cx, int y, uint32_t c, int alpha) { text(f, s, cx - text_w(f, s) / 2, y, c, alpha); }
void text_r(const FontData *f, const char *s, int rx, int y, uint32_t c, int alpha) { text(f, s, rx - text_w(f, s), y, c, alpha); }
void text_shadow(const FontData *f, const char *s, int x, int y, uint32_t c, int alpha) {
    text(f, s, x + 1, y + 2, 0, alpha * 3 / 4);
    text(f, s, x, y, c, alpha);
}

void text_fit(const FontData *f, const char *s, int x, int y, int maxw, uint32_t c, int alpha) {
    if (text_w(f, s) <= maxw) {
        text(f, s, x, y, c, alpha);
        return;
    }
    const unsigned char *dpx, *px;
    const Glyph *dot = glyph(f, 0x2026, &dpx);
    Glyph d = dot ? *dot : (Glyph){ 0 }; /* kept: a later lookup may reuse the cache's slot */
    int dw = dot ? dot->adv : 0, w = 0;
    const char *p = s;
    while (*p) {
        const char *q = p;
        const Glyph *g = glyph(f, utf8(&q), &px);
        int adv = g ? g->adv : 0;
        if (w + adv + dw > maxw) break;
        if (g && g->w) {
            draw_glyph(g, px, x + w, y, c, alpha, 0);
            if (thicken(f, g)) draw_glyph(g, px, x + w + 1, y, c, alpha, 0);
        }
        w += adv, p = q;
    }
    if (dot) draw_glyph(&d, glyph(f, 0x2026, &dpx) ? dpx : 0, x + w, y, c, alpha, 0);
}

/* the title's logo (logo.c), drawn 1:1 with its top-left at x, y */
void logo(int x, int y) {
    for (int j = 0; j < logo_h; ++j)
        for (int i = 0; i < logo_w; ++i) {
            const unsigned char *p = logo_px + 2 * (j * logo_w + i);
            if (p[1]) put(x + i, y + j, (uint32_t)p[0] * 0x010101u, p[1] + (p[1] >> 7));
        }
}

/* an outline in edge, then the letters shaded top to bottom */
void text_grad(const FontData *f, const char *s, int cx, int y, uint32_t top, uint32_t bottom, uint32_t edge) {
    int x0 = cx - text_w(f, s) / 2;
    static const int ox[] = { -2, 2, 0, 0, -2, 2, -2, 2, -1, 1, 0, 0 }, oy[] = { 0, 0, -2, 2, -2, -2, 2, 2, 3, 3, 4, 4 };
    for (int k = 0; k < 12; ++k) text(f, s, x0 + ox[k], y + oy[k], edge, 256);
    const char *p = s;
    int x = x0;
    const unsigned char *px;
    while (*p) {
        const Glyph *g = glyph(f, utf8(&p), &px);
        if (!g || !px) {
            if (g) x += g->adv;
            continue;
        }
        for (int j = 0; j < g->h; ++j) {
            int yy = y + g->by + j;
            uint32_t c = mix(top, bottom, (g->by + j) * 256 / (f->ascent ? f->ascent : 1));
            for (int i = 0; i < g->w; ++i)
                if (px[j * g->w + i]) put(x + g->bx + i, yy, c, px[j * g->w + i]);
        }
        x += g->adv;
    }
}

/* ---------------------------------------------------------------------------------------------
 * Maths */

float fsin(float a) {
    a -= TAU * (float)(int)(a / TAU);
    if (a > PI) a -= TAU;
    if (a < -PI) a += TAU;
    /* sin on -pi..pi: fold to -pi/2..pi/2, then a 7th-order polynomial */
    if (a > PI / 2) a = PI - a;
    if (a < -PI / 2) a = -PI - a;
    float a2 = a * a;
    return a * (1 - a2 * (1.0f / 6 - a2 * (1.0f / 120 - a2 * (1.0f / 5040))));
}
float fcos(float a) { return fsin(a + PI / 2); }

/* ln x: x = m 2^e with m in [1, 2), then ln m by a series in (m - 1) / (m + 1) */
float flog(float x) {
    if (x <= 0) return -80;
    union {
        float f;
        uint32_t u;
    } v = { x };
    int e = (int)(v.u >> 23 & 255) - 127;
    v.u = (v.u & 0x7fffff) | 0x3f800000u;
    float m = v.f;
    if (m > 1.41421356f) m *= 0.5f, ++e;
    float s = (m - 1) / (m + 1), s2 = s * s;
    return e * 0.69314718f + 2 * s * (1 + s2 * (1.0f / 3 + s2 * (1.0f / 5 + s2 * (1.0f / 7))));
}

/* e^x: 2^(x / ln 2), its whole part in the exponent and the rest by a polynomial */
float fexp(float x) {
    if (x < -80) return 0;
    if (x > 80) x = 80;
    float y = x * 1.44269504f;
    int i = (int)(y < 0 ? y - 1 : y);
    float f = y - i, p = 1 + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    union {
        float f;
        uint32_t u;
    } v;
    v.u = (uint32_t)(i + 127) << 23;
    return p * v.f;
}

static unsigned seed = 0x2545f491u;
unsigned rnd(void) {
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return seed;
}
float rndf(void) { return (rnd() >> 8) * (1.0f / 16777216); }

uint32_t fnv(const char *s, uint32_t h) {
    if (!h) h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}
