#include "mp4.h"

#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))
#define MOOV_MAX (16 * 1024 * 1024)

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }

// The raw tables of one track, gathered while walking its boxes.
typedef struct {
    u32 handler, timescale;
    const u8 *stts, *ctts, *stss, *stsc, *stsz, *stco, *co64, *stsd;
    u32 stts_n, ctts_n, stss_n, stsc_n, stco_n, stsd_size;
} Trak;

// Calls fn for each child box in [p, end). Returns 0, or -1 on a bad size.
typedef void (*BoxFn)(u32 type, const u8 *body, u32 size, void *ctx);
static int each_box(const u8 *p, const u8 *end, BoxFn fn, void *ctx)
{
    while (end - p >= 8) {
        u32 size = be32(p), type = be32(p + 4), head = 8;
        if (size == 1) {
            if (end - p < 16 || be32(p + 8)) return -1; // over 4GB
            size = be32(p + 12);
            head = 16;
        } else if (size == 0) {
            size = end - p;
        }
        if (size < head || size > (u32)(end - p)) return -1;
        fn(type, p + head, size - head, ctx);
        p += size;
    }
    return 0;
}

static void walk_trak(u32 type, const u8 *b, u32 n, void *ctx)
{
    Trak *t = ctx;
    switch (type) {
    case FOURCC('m', 'd', 'i', 'a'): case FOURCC('m', 'i', 'n', 'f'): case FOURCC('s', 't', 'b', 'l'):
        each_box(b, b + n, walk_trak, t);
        break;
    case FOURCC('m', 'd', 'h', 'd'):
        if (n >= 24) t->timescale = b[0] == 1 ? (n >= 32 ? be32(b + 20) : 0) : be32(b + 12);
        break;
    case FOURCC('h', 'd', 'l', 'r'):
        if (n >= 12) t->handler = be32(b + 8);
        break;
    case FOURCC('s', 't', 's', 'd'): if (n >= 8) { t->stsd = b + 8; t->stsd_size = n - 8; } break;
    case FOURCC('s', 't', 't', 's'): if (n >= 8) { t->stts = b + 8; t->stts_n = be32(b + 4); } break;
    case FOURCC('c', 't', 't', 's'): if (n >= 8) { t->ctts = b + 8; t->ctts_n = be32(b + 4); } break;
    case FOURCC('s', 't', 's', 's'): if (n >= 8) { t->stss = b + 8; t->stss_n = be32(b + 4); } break;
    case FOURCC('s', 't', 's', 'c'): if (n >= 8) { t->stsc = b + 8; t->stsc_n = be32(b + 4); } break;
    case FOURCC('s', 't', 's', 'z'): if (n >= 12) t->stsz = b; break;
    case FOURCC('s', 't', 'c', 'o'): if (n >= 8) { t->stco = b + 8; t->stco_n = be32(b + 4); } break;
    case FOURCC('c', 'o', '6', '4'): if (n >= 8) { t->co64 = b + 8; t->stco_n = be32(b + 4); } break;
    }
}

typedef struct { Trak traks[8]; int count; } Moov;

static void walk_moov(u32 type, const u8 *b, u32 n, void *ctx)
{
    Moov *m = ctx;
    if (type != FOURCC('t', 'r', 'a', 'k') || m->count == 8) return;
    Trak *t = &m->traks[m->count++];
    memset(t, 0, sizeof(*t));
    each_box(b, b + n, walk_trak, t);
}

// Fills a track's sample tables. Returns 0, or -1 when they don't add up.
static int build_track(Mp4Track *out, const Trak *t, const u8 *end)
{
    if (!t->stsz || !t->stsc || (!t->stco && !t->co64) || !t->stts || !t->timescale) return -1;
    u32 fixed = be32(t->stsz + 4), count = be32(t->stsz + 8);
    if (!count || count > 2000000) return -1;
    if (!fixed && t->stsz + 12 + count * 4 > end) return -1;
    if (t->stsc + t->stsc_n * 12 > end || t->stts + t->stts_n * 8 > end) return -1;
    if ((t->stco ? t->stco + t->stco_n * 4 : t->co64 + t->stco_n * 8) > end) return -1;
    out->count = count;
    out->timescale = t->timescale;
    out->offset = malloc(count * 4);
    out->size = malloc(count * 4);
    out->dts = malloc(count * 4);
    if (!out->offset || !out->size || !out->dts) return -1;
    for (u32 i = 0; i < count; i++) out->size[i] = fixed ? fixed : be32(t->stsz + 12 + i * 4);

    // Chunks: each run in stsc says how many samples the chunks from
    // first_chunk onward hold.
    u32 s = 0;
    for (u32 r = 0; r < t->stsc_n && s < count; r++) {
        u32 first = be32(t->stsc + r * 12) - 1, per = be32(t->stsc + r * 12 + 4);
        u32 last = r + 1 < t->stsc_n ? be32(t->stsc + (r + 1) * 12) - 1 : t->stco_n;
        for (u32 c = first; c < last && c < t->stco_n && s < count; c++) {
            u32 off = t->stco ? be32(t->stco + c * 4) : (be32(t->co64 + c * 8) ? 0xFFFFFFFF : be32(t->co64 + c * 8 + 4));
            for (u32 k = 0; k < per && s < count; k++) {
                out->offset[s] = off;
                off += out->size[s++];
            }
        }
    }
    if (s < count) return -1;

    u32 dts = 0;
    s = 0;
    for (u32 r = 0; r < t->stts_n && s < count; r++) {
        u32 n = be32(t->stts + r * 8), d = be32(t->stts + r * 8 + 4);
        for (u32 k = 0; k < n && s < count; k++, dts += d) out->dts[s++] = dts;
    }
    for (; s < count; s++) out->dts[s] = dts;

    if (t->ctts && t->ctts + t->ctts_n * 8 <= end) {
        out->cts = malloc(count * 4);
        if (!out->cts) return -1;
        s = 0;
        for (u32 r = 0; r < t->ctts_n && s < count; r++) {
            u32 n = be32(t->ctts + r * 8);
            s32 off = (s32)be32(t->ctts + r * 8 + 4);
            for (u32 k = 0; k < n && s < count; k++) out->cts[s++] = off;
        }
        for (; s < count; s++) out->cts[s] = 0;
    }
    if (t->stss && t->stss_n && t->stss + t->stss_n * 4 <= end) {
        out->sync = malloc(t->stss_n * sizeof(int));
        if (!out->sync) return -1;
        out->sync_count = t->stss_n;
        for (u32 i = 0; i < t->stss_n; i++) out->sync[i] = (int)be32(t->stss + i * 4) - 1;
    }
    return 0;
}

// avc1 sample entry: 78 bytes of visual fields, then boxes including avcC.
static void walk_avc1(u32 type, const u8 *b, u32 n, void *ctx)
{
    Mp4 *m = ctx;
    if (type != FOURCC('a', 'v', 'c', 'C') || n < 7 || m->sps) return;
    m->profile = b[1];
    m->nal_length = (b[4] & 3) + 1;
    const u8 *p = b + 5, *end = b + n;
    if ((*p++ & 31) < 1 || end - p < 2) return;
    int sps = be16(p);
    p += 2;
    if (end - p < sps + 3) return;
    const u8 *sps_data = p;
    p += sps;
    if (*p++ < 1) return;
    int pps = be16(p);
    p += 2;
    if (end - p < pps) return;
    m->sps = malloc(sps);
    m->pps = malloc(pps);
    if (!m->sps || !m->pps) return;
    memcpy(m->sps, sps_data, sps);
    memcpy(m->pps, p, pps);
    m->sps_size = sps;
    m->pps_size = pps;
}

// Reads an MPEG-4 descriptor length (1 to 4 bytes of 7 bits).
static u32 desc_len(const u8 **p, const u8 *end)
{
    u32 len = 0;
    for (int i = 0; i < 4 && *p < end; i++) {
        u8 c = *(*p)++;
        len = len << 7 | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return len;
}

// esds: the AudioSpecificConfig gives the real sample rate and channels.
static void walk_mp4a(u32 type, const u8 *b, u32 n, void *ctx)
{
    Mp4 *m = ctx;
    if (type != FOURCC('e', 's', 'd', 's') || n < 4) return;
    const u8 *p = b + 4, *end = b + n;
    if (p >= end || *p++ != 0x03) return;
    desc_len(&p, end);
    if (end - p < 3) return;
    u8 flags = p[2];
    p += 3;
    if (flags & 0x80) p += 2;
    if (flags & 0x40 && p < end) p += 1 + *p;
    if (flags & 0x20) p += 2;
    if (p >= end || *p++ != 0x04) return;
    desc_len(&p, end);
    p += 13;
    if (p >= end || *p++ != 0x05) return;
    desc_len(&p, end);
    if (end - p < 2) return;
    static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    int idx = (p[0] & 7) << 1 | p[1] >> 7, ch = (p[1] >> 3) & 15;
    if (idx < 13) m->sample_rate = rates[idx];
    if (ch) m->channels = ch;
}

int mp4_open(Mp4 *m, const char *path)
{
    memset(m, 0, sizeof(*m));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    // Find moov among the top-level boxes; it may come after the media data.
    u8 head[16];
    u32 pos = 0, moov_at = 0, moov_size = 0;
    SceOff file_size = sceIoLseek(fd, 0, PSP_SEEK_END);
    while ((SceOff)pos + 8 <= file_size) {
        sceIoLseek(fd, pos, PSP_SEEK_SET);
        if (sceIoRead(fd, head, 16) < 8) break;
        u32 size = be32(head), type = be32(head + 4), skip = 8;
        SceOff full = size;
        if (size == 1) { full = (SceOff)be32(head + 8) << 32 | be32(head + 12); skip = 16; }
        else if (size == 0) full = file_size - pos;
        if (full < skip) break;
        if (type == FOURCC('m', 'o', 'o', 'v')) {
            moov_at = pos + skip;
            moov_size = (u32)(full - skip);
            break;
        }
        if ((SceOff)pos + full > 0xFFFFFFFFLL) break;
        pos += (u32)full;
    }
    if (!moov_size || moov_size > MOOV_MAX) { sceIoClose(fd); return -2; }
    u8 *moov = malloc(moov_size);
    if (!moov) { sceIoClose(fd); return -3; }
    sceIoLseek(fd, moov_at, PSP_SEEK_SET);
    int got = sceIoRead(fd, moov, moov_size);
    sceIoClose(fd);
    if (got != (int)moov_size) { free(moov); return -4; }

    Moov mv;
    mv.count = 0;
    each_box(moov, moov + moov_size, walk_moov, &mv);
    const u8 *end = moov + moov_size;
    for (int i = 0; i < mv.count; i++) {
        Trak *t = &mv.traks[i];
        if (!t->stsd || t->stsd_size < 8) continue;
        u32 entry = be32(t->stsd), format = be32(t->stsd + 4);
        if (entry > t->stsd_size) continue;
        const u8 *e = t->stsd + 8, *e_end = t->stsd + entry;
        if (t->handler == FOURCC('v', 'i', 'd', 'e') && !m->has_video && format == FOURCC('a', 'v', 'c', '1') && entry >= 86) {
            m->width = be16(e + 24);
            m->height = be16(e + 26);
            each_box(e + 78, e_end, walk_avc1, m);
            if (m->sps && build_track(&m->video, t, end) == 0) m->has_video = 1;
        } else if (t->handler == FOURCC('s', 'o', 'u', 'n') && !m->has_audio && format == FOURCC('m', 'p', '4', 'a') && entry >= 36) {
            m->channels = be16(e + 16);
            m->sample_rate = be32(e + 24) >> 16;
            each_box(e + 28, e_end, walk_mp4a, m);
            if (build_track(&m->audio, t, end) == 0) m->has_audio = 1;
        }
    }
    free(moov);
    if (!m->has_video) { mp4_close(m); return -5; }
    const Mp4Track *v = &m->video;
    m->duration_ms = mp4_ms(v, v->dts[v->count - 1] + (v->count > 1 ? v->dts[v->count - 1] - v->dts[v->count - 2] : 0));
    return 0;
}

static void free_track(Mp4Track *t)
{
    free(t->offset);
    free(t->size);
    free(t->dts);
    free(t->cts);
    free(t->sync);
}

void mp4_close(Mp4 *m)
{
    free_track(&m->video);
    free_track(&m->audio);
    free(m->sps);
    free(m->pps);
    memset(m, 0, sizeof(*m));
}

u32 mp4_ms(const Mp4Track *t, u32 ticks)
{
    return t->timescale ? (u32)((u64)ticks * 1000 / t->timescale) : 0;
}

int mp4_sample_at(const Mp4Track *t, u32 ms)
{
    u32 ticks = (u32)((u64)ms * t->timescale / 1000);
    int lo = 0, hi = t->count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (t->dts[mid] <= ticks) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

int mp4_keyframe_before(const Mp4 *m, int sample)
{
    const Mp4Track *v = &m->video;
    if (!v->sync) return sample;
    int best = v->sync_count ? v->sync[0] : 0;
    for (int i = 0; i < v->sync_count && v->sync[i] <= sample; i++) best = v->sync[i];
    return best;
}
