#include "m4b.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

// Byte by byte: odd offsets must never be read as whole words on the PSP.
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }

typedef struct { u32 first_chunk, per, first_sample; } StscRun;
typedef struct { u32 count, delta; } SttsRun;

// One track's tables: small ones in memory, big ones (sizes, chunk offsets)
// left in the file.
typedef struct {
    u32 id, handler, timescale, chap_ref;
    u32 count, fixed, stsz_pos;   // sample sizes
    u32 chunks, stco_pos; int co64;
    StscRun *stsc; u32 stsc_n;
    SttsRun *stts; u32 stts_n;
    int rate, channels;
} Trak;

typedef struct {
    SceUID fd;
    Trak traks[6];
    int trak_n;
    Trak *cur;
    M4bInfo *info; // filled while walking, when asked
    u32 chpl_pos, chpl_size;
} Walk;

static int read_at(SceUID fd, u32 pos, void *buf, u32 n)
{
    if (sceIoLseek32(fd, pos, PSP_SEEK_SET) != (int)pos) return -1;
    return sceIoRead(fd, buf, n) == (int)n ? 0 : -1;
}

static u8 *load(SceUID fd, u32 pos, u32 n)
{
    if (n > 4 * 1024 * 1024) return NULL;
    u8 *b = malloc(n ? n : 1);
    if (b && read_at(fd, pos, b, n) < 0) { free(b); b = NULL; }
    return b;
}

static void parse_mp4a(Trak *t, const u8 *b, u32 n)
{
    // stsd: version/flags, count, then the first entry.
    if (n < 8 + 36 || be32(b + 12) != FOURCC('m', 'p', '4', 'a')) return;
    const u8 *e = b + 8, *end = b + n;
    t->channels = be16(e + 24);
    t->rate = be32(e + 32) >> 16;
    // esds inside: the AudioSpecificConfig has the real rate.
    for (const u8 *p = e + 36; p + 8 <= end;) {
        u32 size = be32(p);
        if (size < 8 || p + size > end) break;
        if (be32(p + 4) == FOURCC('e', 's', 'd', 's')) {
            for (const u8 *q = p + 12; q + 2 < p + size; q++)
                if (q[0] == 0x05) { // DecoderSpecificInfo tag
                    const u8 *r = q + 1;
                    for (int k = 0; k < 4 && r < p + size && (*r & 0x80); k++) r++;
                    r++;
                    if (r + 2 <= p + size) {
                        static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
                        int idx = (r[0] & 7) << 1 | r[1] >> 7, ch = (r[1] >> 3) & 15;
                        if (idx < 13) t->rate = rates[idx];
                        if (ch) t->channels = ch;
                    }
                    break;
                }
        }
        p += size;
    }
}

static void tag_text(char *out, int size, const u8 *b, u32 n)
{
    // ilst item: a 'data' box with type, locale, then the text.
    if (n < 16 || be32(b + 4) != FOURCC('d', 'a', 't', 'a')) return;
    u32 len = be32(b) - 16;
    if (len > n - 16) len = n - 16;
    if ((int)len >= size) len = size - 1;
    memcpy(out, b + 16, len);
    out[len] = 0;
}

static void walk(Walk *w, u32 pos, u32 end, u32 parent);

static void box(Walk *w, u32 type, u32 body, u32 n, u32 parent)
{
    SceUID fd = w->fd;
    Trak *t = w->cur;
    switch (type) {
    case FOURCC('m', 'o', 'o', 'v'): case FOURCC('m', 'd', 'i', 'a'): case FOURCC('m', 'i', 'n', 'f'):
    case FOURCC('s', 't', 'b', 'l'): case FOURCC('u', 'd', 't', 'a'): case FOURCC('t', 'r', 'e', 'f'):
    case FOURCC('i', 'l', 's', 't'):
        walk(w, body, body + n, type);
        break;
    case FOURCC('m', 'e', 't', 'a'):
        walk(w, body + 4, body + n, type); // a full box
        break;
    case FOURCC('t', 'r', 'a', 'k'):
        if (w->trak_n == 6) break;
        w->cur = &w->traks[w->trak_n++];
        memset(w->cur, 0, sizeof(Trak));
        walk(w, body, body + n, type);
        w->cur = NULL;
        break;
    case FOURCC('c', 'h', 'p', 'l'):
        w->chpl_pos = body;
        w->chpl_size = n;
        break;
    }
    if (parent == FOURCC('i', 'l', 's', 't') && w->info && n <= 64 * 1024 * 1024) {
        M4bInfo *in = w->info;
        if (type == FOURCC('c', 'o', 'v', 'r')) {
            u8 h[16];
            if (n >= 16 && read_at(fd, body, h, 16) == 0 && be32(h + 4) == FOURCC('d', 'a', 't', 'a')) {
                in->cover_offset = body + 16;
                in->cover_size = be32(h) - 16;
                in->cover_png = be32(h + 8) == 14;
            }
        } else if (n <= 1024 && (type == FOURCC(0xA9, 'a', 'l', 'b') || type == FOURCC(0xA9, 'n', 'a', 'm') ||
                                 type == FOURCC(0xA9, 'A', 'R', 'T') || type == FOURCC('a', 'A', 'R', 'T'))) {
            u8 *b = load(fd, body, n);
            if (b) {
                char text[128] = "";
                tag_text(text, sizeof(text), b, n);
                if (type == FOURCC(0xA9, 'a', 'l', 'b') && text[0]) snprintf(in->title, sizeof(in->title), "%s", text);
                if (type == FOURCC(0xA9, 'n', 'a', 'm') && text[0] && !in->title[0]) snprintf(in->title, sizeof(in->title), "%s", text);
                if (type == FOURCC('a', 'A', 'R', 'T') && text[0]) snprintf(in->author, sizeof(in->author), "%s", text);
                if (type == FOURCC(0xA9, 'A', 'R', 'T') && text[0] && !in->author[0]) snprintf(in->author, sizeof(in->author), "%s", text);
                free(b);
            }
        }
    }
    if (!t) return;
    u8 head[16];
    switch (type) {
    case FOURCC('t', 'k', 'h', 'd'):
        // Version 0: times are 4 bytes and the id is at 12; version 1: 8 bytes, id at 20.
        if (n >= 24 && read_at(fd, body, head, 16) == 0) t->id = be32(head + 12);
        if (n >= 32 && head[0] == 1) { u8 v[4]; if (read_at(fd, body + 20, v, 4) == 0) t->id = be32(v); }
        break;
    case FOURCC('c', 'h', 'a', 'p'):
        if (n >= 4 && read_at(fd, body, head, 4) == 0) t->chap_ref = be32(head);
        break;
    case FOURCC('m', 'd', 'h', 'd'):
        if (n >= 24 && read_at(fd, body, head, 16) == 0) {
            if (head[0] == 1) { u8 v[4]; if (read_at(fd, body + 20, v, 4) == 0) t->timescale = be32(v); }
            else t->timescale = be32(head + 12);
        }
        break;
    case FOURCC('h', 'd', 'l', 'r'):
        if (n >= 12 && read_at(fd, body, head, 12) == 0) t->handler = be32(head + 8);
        break;
    case FOURCC('s', 't', 's', 'd'):
        if (n <= 4096) {
            u8 *b = load(fd, body, n);
            if (b) { parse_mp4a(t, b, n); free(b); }
        }
        break;
    case FOURCC('s', 't', 't', 's'):
        if (n >= 8 && read_at(fd, body, head, 8) == 0) {
            u32 k = be32(head + 4);
            u8 *b = k && k < 100000 && 8 + k * 8 <= n ? load(fd, body + 8, k * 8) : NULL;
            if (b) {
                t->stts = malloc(k * sizeof(SttsRun));
                if (t->stts) {
                    for (u32 i = 0; i < k; i++) t->stts[i] = (SttsRun){ be32(b + i * 8), be32(b + i * 8 + 4) };
                    t->stts_n = k;
                }
                free(b);
            }
        }
        break;
    case FOURCC('s', 't', 's', 'c'):
        if (n >= 8 && read_at(fd, body, head, 8) == 0) {
            u32 k = be32(head + 4);
            u8 *b = k && k < 200000 && 8 + k * 12 <= n ? load(fd, body + 8, k * 12) : NULL;
            if (b) {
                t->stsc = malloc(k * sizeof(StscRun));
                if (t->stsc) {
                    for (u32 i = 0; i < k; i++) t->stsc[i] = (StscRun){ be32(b + i * 12) - 1, be32(b + i * 12 + 4), 0 };
                    t->stsc_n = k;
                }
                free(b);
            }
        }
        break;
    case FOURCC('s', 't', 's', 'z'):
        if (n >= 12 && read_at(fd, body, head, 12) == 0) {
            t->fixed = be32(head + 4);
            t->count = be32(head + 8);
            t->stsz_pos = body + 12;
            if (!t->fixed && 12 + (u64)t->count * 4 > n) t->count = 0;
        }
        break;
    case FOURCC('s', 't', 'c', 'o'): case FOURCC('c', 'o', '6', '4'):
        if (n >= 8 && read_at(fd, body, head, 8) == 0) {
            t->chunks = be32(head + 4);
            t->co64 = type == FOURCC('c', 'o', '6', '4');
            t->stco_pos = body + 8;
            if (8 + (u64)t->chunks * (t->co64 ? 8 : 4) > n) t->chunks = 0;
        }
        break;
    }
}

static void walk(Walk *w, u32 pos, u32 end, u32 parent)
{
    u8 h[16];
    while (pos + 8 <= end) {
        if (read_at(w->fd, pos, h, 8) < 0) return;
        u32 size = be32(h), type = be32(h + 4), head = 8;
        if (size == 1) {
            if (read_at(w->fd, pos + 8, h + 8, 8) < 0 || be32(h + 8)) return; // over 4GB
            size = be32(h + 12);
            head = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < head || size > end - pos) return;
        // At the top only moov matters (mdat is skipped unread).
        if (parent || type == FOURCC('m', 'o', 'o', 'v')) box(w, type, pos + head, size - head, parent);
        pos += size;
    }
}

static void free_traks(Walk *w)
{
    for (int i = 0; i < w->trak_n; i++) { free(w->traks[i].stsc); free(w->traks[i].stts); }
}

// Fills first_sample for each stsc run. Returns 0 when the tables agree.
static int check(Trak *t)
{
    if (!t->count || !t->chunks || !t->stsc_n || !t->stts_n || !t->timescale) return -1;
    u32 s = 0;
    for (u32 r = 0; r < t->stsc_n; r++) {
        t->stsc[r].first_sample = s;
        u32 last = r + 1 < t->stsc_n ? t->stsc[r + 1].first_chunk : t->chunks;
        if (last < t->stsc[r].first_chunk) return -1;
        s += (last - t->stsc[r].first_chunk) * t->stsc[r].per;
    }
    return s >= t->count ? 0 : -1;
}

static Trak *find(Walk *w, u32 handler)
{
    for (int i = 0; i < w->trak_n; i++)
        if (w->traks[i].handler == handler && check(&w->traks[i]) == 0) return &w->traks[i];
    return NULL;
}

static u32 sample_ms(const Trak *t, u32 sample)
{
    u64 ticks = 0;
    for (u32 r = 0; r < t->stts_n && sample; r++) {
        u32 k = t->stts[r].count < sample ? t->stts[r].count : sample;
        ticks += (u64)k * t->stts[r].delta;
        sample -= k;
    }
    return (u32)(ticks * 1000 / t->timescale);
}

// ---- Sample positions, straight from the file -----------------------------------------

static u32 sample_size(SceUID fd, const Trak *t, u32 s)
{
    if (t->fixed) return t->fixed;
    u8 b[4];
    return read_at(fd, t->stsz_pos + s * 4, b, 4) == 0 ? be32(b) : 0;
}

static u32 chunk_offset(SceUID fd, const Trak *t, u32 c)
{
    u8 b[8];
    if (t->co64) return read_at(fd, t->stco_pos + c * 8, b, 8) == 0 && !be32(b) ? be32(b + 4) : 0;
    return read_at(fd, t->stco_pos + c * 4, b, 4) == 0 ? be32(b) : 0;
}

// The file offset of sample s.
static u32 sample_offset(SceUID fd, const Trak *t, u32 s)
{
    u32 r = 0;
    while (r + 1 < t->stsc_n && t->stsc[r + 1].first_sample <= s) r++;
    const StscRun *run = &t->stsc[r];
    u32 per = run->per ? run->per : 1, chunk = run->first_chunk + (s - run->first_sample) / per;
    u32 first = run->first_sample + (chunk - run->first_chunk) * per;
    u32 off = chunk_offset(fd, t, chunk);
    if (t->fixed) return off + (s - first) * t->fixed;
    u8 raw[1024];
    for (u32 k = first; k < s;) {
        u32 n = s - k < 256 ? s - k : 256;
        if (read_at(fd, t->stsz_pos + k * 4, raw, n * 4) < 0) return off;
        for (u32 i = 0; i < n; i++) off += be32(raw + i * 4);
        k += n;
    }
    return off;
}

// ---- Info ------------------------------------------------------------------------------

static void chapters_chpl(Walk *w, M4bInfo *in)
{
    if (!w->chpl_size || w->chpl_size > 64 * 1024) return;
    u8 *b = load(w->fd, w->chpl_pos, w->chpl_size);
    if (!b) return;
    const u8 *p = b + 4, *end = b + w->chpl_size;
    if (b[0]) p += 4;
    int n = p < end ? *p++ : 0;
    for (int i = 0; i < n && in->chapter_count < M4B_CHAPTERS && p + 9 <= end; i++) {
        u64 start = (u64)be32(p) << 32 | be32(p + 4); // 100 ns units
        int len = p[8];
        p += 9;
        if (p + len > end) break;
        int k = in->chapter_count++;
        in->chapter_ms[k] = (u32)(start / 10000);
        int m = len < 63 ? len : 63;
        memcpy(in->chapter_name[k], p, m);
        in->chapter_name[k][m] = 0;
        p += len;
    }
    free(b);
}

static void chapters_track(Walk *w, const Trak *audio, M4bInfo *in)
{
    if (!audio->chap_ref) return;
    Trak *t = NULL;
    for (int i = 0; i < w->trak_n; i++)
        if (w->traks[i].id == audio->chap_ref && check(&w->traks[i]) == 0) t = &w->traks[i];
    if (!t) return;
    for (u32 s = 0; s < t->count && in->chapter_count < M4B_CHAPTERS; s++) {
        u32 size = sample_size(w->fd, t, s);
        u8 b[66];
        int k = in->chapter_count;
        in->chapter_name[k][0] = 0;
        if (size >= 2 && read_at(w->fd, sample_offset(w->fd, t, s), b, size < 66 ? size : 66) == 0) {
            u32 len = be16(b);
            if (len > 63) len = 63;
            if (len + 2 > size) len = size > 2 ? size - 2 : 0;
            memcpy(in->chapter_name[k], b + 2, len);
            in->chapter_name[k][len] = 0;
        }
        in->chapter_ms[k] = sample_ms(t, s);
        in->chapter_count++;
    }
}

int m4b_info(const char *path, M4bInfo *in)
{
    memset(in, 0, sizeof(*in));
    Walk w;
    memset(&w, 0, sizeof(w));
    w.fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (w.fd < 0) return -1;
    w.info = in;
    u32 size = sceIoLseek32(w.fd, 0, PSP_SEEK_END);
    walk(&w, 0, size, 0);
    Trak *a = find(&w, FOURCC('s', 'o', 'u', 'n'));
    int ok = a && a->rate > 0;
    if (ok) {
        in->rate = a->rate;
        in->channels = a->channels;
        in->duration_ms = sample_ms(a, a->count);
        chapters_chpl(&w, in);
        if (!in->chapter_count) chapters_track(&w, a, in);
        // Chapters must start in order inside the book.
        int k = 0;
        for (int i = 0; i < in->chapter_count; i++)
            if (in->chapter_ms[i] < in->duration_ms && (!k || in->chapter_ms[i] > in->chapter_ms[k - 1])) {
                if (k != i) { in->chapter_ms[k] = in->chapter_ms[i]; memcpy(in->chapter_name[k], in->chapter_name[i], 64); }
                k++;
            }
        in->chapter_count = k;
        if (k) in->chapter_ms[0] = 0;
    }
    free_traks(&w);
    sceIoClose(w.fd);
    return ok ? 0 : -1;
}

// ---- Playing -----------------------------------------------------------------------------

struct M4b {
    Walk w;
    Trak *t;
    u32 sample, offset; // next frame to read and where it starts
    u32 run, chunk, left_in_chunk;
    u32 sizes[256], sizes_first, sizes_n; // a window of stsz
    u32 max_frame;
};

M4b *m4b_open(const char *path)
{
    M4b *m = calloc(1, sizeof(M4b));
    if (!m) return NULL;
    m->w.fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (m->w.fd < 0) { free(m); return NULL; }
    u32 size = sceIoLseek32(m->w.fd, 0, PSP_SEEK_END);
    walk(&m->w, 0, size, 0);
    m->t = find(&m->w, FOURCC('s', 'o', 'u', 'n'));
    if (!m->t || m->t->rate <= 0) { m4b_close(m); return NULL; }
    m->max_frame = m->t->fixed ? m->t->fixed : 8192; // AAC frames stay far below this
    m4b_seek(m, 0);
    return m;
}

void m4b_close(M4b *m)
{
    if (!m) return;
    free_traks(&m->w);
    if (m->w.fd >= 0) sceIoClose(m->w.fd);
    free(m);
}

int m4b_rate(const M4b *m) { return m->t->rate; }
u32 m4b_max_frame(const M4b *m) { return m->max_frame; }

u32 m4b_seek(M4b *m, u32 ms)
{
    const Trak *t = m->t;
    // The frame holding ms, from the time table.
    u64 want = (u64)ms * t->timescale / 1000, ticks = 0;
    u32 s = 0;
    for (u32 r = 0; r < t->stts_n; r++) {
        u64 span = (u64)t->stts[r].count * t->stts[r].delta;
        if (ticks + span > want && t->stts[r].delta) { s += (u32)((want - ticks) / t->stts[r].delta); break; }
        ticks += span;
        s += t->stts[r].count;
    }
    if (s >= t->count) s = t->count ? t->count - 1 : 0;
    m->sample = s;
    m->offset = sample_offset(m->w.fd, t, s);
    m->run = 0;
    while (m->run + 1 < t->stsc_n && t->stsc[m->run + 1].first_sample <= s) m->run++;
    const StscRun *run = &t->stsc[m->run];
    u32 per = run->per ? run->per : 1;
    m->chunk = run->first_chunk + (s - run->first_sample) / per;
    m->left_in_chunk = per - (s - run->first_sample) % per;
    m->sizes_n = 0;
    return sample_ms(t, s);
}

int m4b_next(M4b *m, u8 *buf, u32 cap)
{
    const Trak *t = m->t;
    if (m->sample >= t->count) return 0;
    if (!m->left_in_chunk) {
        m->chunk++;
        if (m->run + 1 < t->stsc_n && m->chunk >= t->stsc[m->run + 1].first_chunk) m->run++;
        m->left_in_chunk = t->stsc[m->run].per ? t->stsc[m->run].per : 1;
        m->offset = chunk_offset(m->w.fd, t, m->chunk);
    }
    u32 size;
    if (t->fixed) {
        size = t->fixed;
    } else {
        if (m->sample < m->sizes_first || m->sample >= m->sizes_first + m->sizes_n) {
            u32 n = t->count - m->sample < 256 ? t->count - m->sample : 256;
            u8 raw[256 * 4];
            if (read_at(m->w.fd, t->stsz_pos + m->sample * 4, raw, n * 4) < 0) return -1;
            for (u32 i = 0; i < n; i++) m->sizes[i] = be32(raw + i * 4);
            m->sizes_first = m->sample;
            m->sizes_n = n;
        }
        size = m->sizes[m->sample - m->sizes_first];
    }
    if (!size || size > cap) return -1;
    if (read_at(m->w.fd, m->offset, buf, size) < 0) return -1;
    m->offset += size;
    m->sample++;
    m->left_in_chunk--;
    return (int)size;
}
