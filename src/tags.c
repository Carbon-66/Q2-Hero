/* A song's title and artist from its own tags, read here with no process to start, so a folder's
 * names come quickly:
 *
 *   ID3v2 (2.2, 2.3, 2.4)  MP3, and inside AIFF, WAV and DSF; ID3v1 at an MP3's end
 *   Vorbis comments        FLAC; Ogg Vorbis and Opus (their second packet)
 *   MP4 atoms              M4A, AAC, ALAC (moov.udta.meta.ilst: (c)nam, (c)ART)
 *   APEv2                  APE, WavPack, Musepack, and some MP3s
 *   RIFF INFO              WAV (INAM, IART)
 *
 * Text comes out as UTF-8. ID3's "Latin-1" that is really UTF-8 (a common slip) is taken as
 * UTF-8. Frames are read one at a time and the rest skipped, so a cover picture costs a seek. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "hero.h"

typedef struct {
    int fd;
    long size;
    char *title, *artist;
    int tn, an;
} Src;

static int rd(Src *s, long at, void *buf, int n) {
    if (at < 0 || at + n > s->size || lseek(s->fd, (off_t)at, SEEK_SET) != (off_t)at) return 0;
    return read(s->fd, buf, (size_t)n) == n;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint32_t sync32(const uint8_t *p) { return (uint32_t)(p[0] & 127) << 21 | (uint32_t)(p[1] & 127) << 14 | (uint32_t)(p[2] & 127) << 7 | (p[3] & 127); }

/* ---------------------------------------------------------------------------------------------
 * Text into UTF-8 */
static int put8(char *out, int at, int cap, unsigned c) {
    char b[4];
    int n = c < 0x80 ? (b[0] = (char)c, 1)
          : c < 0x800 ? (b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), 2)
          : c < 0x10000 ? (b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), 3)
          : (b[0] = (char)(0xf0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 63)), b[2] = (char)(0x80 | (c >> 6 & 63)), b[3] = (char)(0x80 | (c & 63)), 4);
    if (at + n >= cap) return at;
    memcpy(out + at, b, (size_t)n);
    return at + n;
}

static int valid_utf8_multi(const uint8_t *p, int n) { /* valid UTF-8, with a multi-byte sequence */
    int multi = 0;
    for (int i = 0; i < n;) {
        uint8_t c = p[i];
        int k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!k || i + k > n) return 0;
        for (int j = 1; j < k; ++j)
            if ((p[i + j] & 0xc0) != 0x80) return 0;
        multi |= k > 1;
        i += k;
    }
    return multi;
}

/* enc: 0 Latin-1, 1 UTF-16 with a BOM, 2 UTF-16BE, 3 UTF-8, 4 UTF-16LE */
static void decode(const uint8_t *p, int n, int enc, char *out, int cap) {
    int o = 0;
    if (enc == 0 && valid_utf8_multi(p, n)) enc = 3;
    if (enc == 1 && n >= 2) {
        enc = p[0] == 0xff && p[1] == 0xfe ? 4 : 2;
        if ((p[0] == 0xff && p[1] == 0xfe) || (p[0] == 0xfe && p[1] == 0xff)) p += 2, n -= 2;
    }
    if (enc == 2 || enc == 4) {
        for (int i = 0; i + 1 < n; i += 2) {
            unsigned c = enc == 2 ? (unsigned)p[i] << 8 | p[i + 1] : (unsigned)p[i + 1] << 8 | p[i];
            if (!c) break;
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
                unsigned d = enc == 2 ? (unsigned)p[i + 2] << 8 | p[i + 3] : (unsigned)p[i + 3] << 8 | p[i + 2];
                if (d >= 0xdc00 && d < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00), i += 2;
            }
            o = put8(out, o, cap, c);
        }
    } else if (enc == 3) {
        for (int i = 0; i < n && p[i] && o + 1 < cap; ++i) out[o++] = (char)p[i];
    } else
        for (int i = 0; i < n && p[i]; ++i) o = put8(out, o, cap, p[i]);
    out[o] = 0;
    /* no tabs or line ends: the names go into a one-line list and a tab-separated cache */
    for (char *c = out; *c; ++c)
        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
}

/* ---------------------------------------------------------------------------------------------
 * ID3v2 at at */
static int id3v2(Src *s, long at) {
    uint8_t h[10];
    if (!rd(s, at, h, 10) || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4) return 0;
    int ver = h[3], flags = h[5];
    long end = at + 10 + sync32(h + 6), p = at + 10;
    if (ver == 3 && flags & 0x80) return 0; /* unsynchronised as a whole: rare enough to leave */
    if (flags & 0x40 && ver >= 3) { /* an extended header */
        uint8_t e[4];
        if (!rd(s, p, e, 4)) return 0;
        p += ver == 4 ? (long)sync32(e) : 4 + (long)be32(e);
    }
    int found = 0;
    while (p + (ver == 2 ? 6 : 10) <= end && found < 2) {
        uint8_t f[10];
        int hl = ver == 2 ? 6 : 10;
        if (!rd(s, p, f, hl) || !f[0]) break;
        long n = ver == 2 ? (long)f[3] << 16 | f[4] << 8 | f[5] : ver == 4 ? (long)sync32(f + 4) : (long)be32(f + 4);
        if (n <= 0 || p + hl + n > end) break;
        int title = ver == 2 ? !memcmp(f, "TT2", 3) : !memcmp(f, "TIT2", 4);
        int artist = ver == 2 ? !memcmp(f, "TP1", 3) : !memcmp(f, "TPE1", 4);
        int flagged = ver == 4 && (f[9] & 0x0f); /* compressed, encrypted, unsynchronised or with a length: skipped */
        if ((title || artist) && !flagged && n < 4096) {
            uint8_t b[4096];
            if (rd(s, p + hl, b, (int)n)) {
                if (title && !s->title[0]) decode(b + 1, (int)n - 1, b[0], s->title, s->tn), ++found;
                if (artist && !s->artist[0]) decode(b + 1, (int)n - 1, b[0], s->artist, s->an), ++found;
            }
        }
        p += hl + n;
    }
    return s->title[0] != 0;
}

static int id3v1(Src *s) {
    uint8_t t[128];
    if (s->size < 128 || !rd(s, s->size - 128, t, 128) || memcmp(t, "TAG", 3)) return 0;
    if (!s->title[0]) decode(t + 3, 30, 0, s->title, s->tn);
    if (!s->artist[0]) decode(t + 33, 30, 0, s->artist, s->an);
    return s->title[0] != 0;
}

/* ---------------------------------------------------------------------------------------------
 * Vorbis comments in b[0..n) */
static int vorbis_comments(Src *s, const uint8_t *b, long n) {
    if (n < 8) return 0;
    long p = 4 + (long)le32(b);
    if (p + 4 > n) return 0;
    uint32_t count = le32(b + p);
    p += 4;
    for (uint32_t i = 0; i < count && p + 4 <= n; ++i) {
        long len = (long)le32(b + p);
        p += 4;
        if (len < 0 || p + len > n) break;
        const char *c = (const char *)b + p;
        if (len > 6 && !strncasecmp(c, "TITLE=", 6) && !s->title[0]) decode(b + p + 6, (int)len - 6, 3, s->title, s->tn);
        else if (len > 7 && !strncasecmp(c, "ARTIST=", 7) && !s->artist[0]) decode(b + p + 7, (int)len - 7, 3, s->artist, s->an);
        else if (len > 12 && !strncasecmp(c, "ALBUMARTIST=", 12) && !s->artist[0]) decode(b + p + 12, (int)len - 12, 3, s->artist, s->an);
        p += len;
    }
    return s->title[0] != 0;
}

static int flac(Src *s) {
    long p = 4;
    for (int k = 0; k < 64; ++k) {
        uint8_t h[4];
        if (!rd(s, p, h, 4)) return 0;
        long n = (long)h[1] << 16 | h[2] << 8 | h[3];
        if ((h[0] & 127) == 4 && n < (1 << 20)) {
            uint8_t *b = malloc((size_t)n);
            int ok = b && rd(s, p + 4, b, (int)n) && vorbis_comments(s, b, n);
            free(b);
            return ok;
        }
        if (h[0] & 128) return 0; /* the last block */
        p += 4 + n;
    }
    return 0;
}

/* Ogg: the second packet, from the pages' payloads, read up to 256 KB */
static int ogg(Src *s) {
    long cap = 256 * 1024, got = 0, p = 0;
    uint8_t *pk = malloc((size_t)cap);
    int packet = 0, ok = 0;
    if (!pk) return 0;
    while (got < cap && packet < 2) {
        uint8_t h[27 + 255];
        if (!rd(s, p, h, 27) || memcmp(h, "OggS", 4) || !rd(s, p + 27, h + 27, h[26])) break;
        int segs = h[26];
        long body = p + 27 + segs;
        for (int i = 0; i < segs && packet < 2; ++i) {
            int len = h[27 + i];
            if (packet == 1) {
                if (got + len > cap || !rd(s, body, pk + got, len)) {
                    packet = 2;
                    break;
                }
                got += len;
            }
            body += len;
            if (len < 255) ++packet; /* a packet ends */
        }
        for (int i = 0; i < segs; ++i) p += h[27 + i];
        p += 27 + segs;
    }
    if (got > 7 && !memcmp(pk, "\x03vorbis", 7)) ok = vorbis_comments(s, pk + 7, got - 7);
    else if (got > 8 && !memcmp(pk, "OpusTags", 8)) ok = vorbis_comments(s, pk + 8, got - 8);
    free(pk);
    return ok;
}

/* ---------------------------------------------------------------------------------------------
 * MP4: boxes from a to b, the path moov udta meta ilst */
static int mp4_box(Src *s, long a, long b, int depth) {
    long p = a;
    while (p + 8 <= b) {
        uint8_t h[16];
        if (!rd(s, p, h, 8)) return 0;
        long n = (long)be32(h), hl = 8;
        if (n == 1) { /* a 64-bit size */
            if (!rd(s, p + 8, h + 8, 8) || be32(h + 8)) return 0;
            n = (long)be32(h + 12), hl = 16;
        } else if (n == 0)
            n = b - p;
        if (n < hl || p + n > b) return 0;
        const char *t = (const char *)h + 4;
        if (depth == 0 && !memcmp(t, "moov", 4)) return mp4_box(s, p + hl, p + n, 1);
        if (depth == 1 && !memcmp(t, "udta", 4)) return mp4_box(s, p + hl, p + n, 2);
        if (depth == 2 && !memcmp(t, "meta", 4)) return mp4_box(s, p + hl + 4, p + n, 3); /* a full box */
        if (depth == 3 && !memcmp(t, "ilst", 4)) return mp4_box(s, p + hl, p + n, 4);
        if (depth == 4) {
            int title = !memcmp(t, "\xa9nam", 4), artist = !memcmp(t, "\xa9" "ART", 4) || !memcmp(t, "aART", 4);
            uint8_t d[16 + 1024];
            long dn = n - hl;
            if ((title || artist) && dn > 16 && dn <= (long)sizeof d && rd(s, p + hl, d, (int)dn) && !memcmp(d + 4, "data", 4)) {
                long len = (long)be32(d) - 16;
                if (len > 0 && len <= dn - 16) {
                    if (title && !s->title[0]) decode(d + 16, (int)len, 3, s->title, s->tn);
                    if (artist && !s->artist[0]) decode(d + 16, (int)len, 3, s->artist, s->an);
                }
            }
        }
        p += n;
    }
    return s->title[0] != 0;
}

/* ---------------------------------------------------------------------------------------------
 * APEv2: a footer 32 bytes before the end, or before an ID3v1 */
static int ape(Src *s) {
    for (int tail = 0; tail <= 128; tail += 128) {
        uint8_t f[32];
        long at = s->size - tail - 32;
        if (!rd(s, at, f, 32) || memcmp(f, "APETAGEX", 8)) continue;
        long n = (long)le32(f + 12), count = (long)le32(f + 16);
        if (n < 32 || n > (1 << 20) || at + 32 - n < 0) return 0;
        uint8_t *b = malloc((size_t)n);
        if (!b || !rd(s, at + 32 - n, b, (int)n - 32)) return free(b), 0;
        long p = 0;
        for (long i = 0; i < count && p + 8 < n - 32; ++i) {
            long len = (long)le32(b + p);
            const char *key = (const char *)b + p + 8;
            long kl = (long)strnlen(key, (size_t)(n - 32 - p - 8));
            long v = p + 8 + kl + 1;
            if (len < 0 || v + len > n - 32) break;
            if (!strcasecmp(key, "Title") && !s->title[0]) decode(b + v, (int)len, 3, s->title, s->tn);
            if (!strcasecmp(key, "Artist") && !s->artist[0]) decode(b + v, (int)len, 3, s->artist, s->an);
            p = v + len;
        }
        free(b);
        return s->title[0] != 0;
    }
    return 0;
}

/* RIFF WAVE and FORM AIFF: chunks; an id3 chunk, or WAVE's LIST INFO */
static int chunks(Src *s, int riff) {
    long p = 12;
    for (int k = 0; k < 256 && p + 8 <= s->size; ++k) {
        uint8_t h[8];
        if (!rd(s, p, h, 8)) return 0;
        long n = riff ? (long)le32(h + 4) : (long)be32(h + 4);
        if (n < 0 || p + 8 + n > s->size) return 0;
        if (!strncasecmp((const char *)h, "id3 ", 4) && id3v2(s, p + 8)) return 1;
        if (riff && !memcmp(h, "LIST", 4) && n > 4 && n < 65536) {
            uint8_t *b = malloc((size_t)n);
            if (b && rd(s, p + 8, b, (int)n) && !memcmp(b, "INFO", 4))
                for (long q = 4; q + 8 <= n;) {
                    long len = (long)le32(b + q + 4);
                    if (len < 0 || q + 8 + len > n) break;
                    if (!memcmp(b + q, "INAM", 4) && !s->title[0]) decode(b + q + 8, (int)len, 0, s->title, s->tn);
                    if (!memcmp(b + q, "IART", 4) && !s->artist[0]) decode(b + q + 8, (int)len, 0, s->artist, s->an);
                    q += 8 + len + (len & 1);
                }
            free(b);
        }
        p += 8 + n + (n & 1);
    }
    return s->title[0] != 0;
}

int tags_read(const char *path, char *title, int tn, char *artist, int an) {
    Src s = { open(path, O_RDONLY | O_CLOEXEC), 0, title, artist, tn, an };
    title[0] = artist[0] = 0;
    if (s.fd < 0) return 0;
    s.size = (long)lseek(s.fd, 0, SEEK_END);
    uint8_t h[16];
    int ok = 0;
    if (rd(&s, 0, h, 12)) {
        if (!memcmp(h, "ID3", 3)) ok = id3v2(&s, 0);
        else if (!memcmp(h, "fLaC", 4)) ok = flac(&s);
        else if (!memcmp(h, "OggS", 4)) ok = ogg(&s);
        else if (!memcmp(h + 4, "ftyp", 4)) ok = mp4_box(&s, 0, s.size, 0);
        else if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) ok = chunks(&s, 1);
        else if (!memcmp(h, "FORM", 4) && (!memcmp(h + 8, "AIFF", 4) || !memcmp(h + 8, "AIFC", 4))) ok = chunks(&s, 0);
        else if (!memcmp(h, "DSD ", 4)) {
            uint8_t m[8];
            ok = rd(&s, 20, m, 8) && !be32(m + 4) && le32(m) && id3v2(&s, (long)le32(m));
        }
        if (!ok) ok = ape(&s);
        if (!ok) ok = id3v1(&s);
    }
    close(s.fd);
    return ok;
}
