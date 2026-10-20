#include "tags.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static u32 syncsafe(const u8 *p) { return (p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

static int put_utf8(char *out, int pos, int cap, u32 c)
{
    if (c < 0x80) { if (pos + 1 < cap) out[pos++] = c; }
    else if (c < 0x800) { if (pos + 2 < cap) { out[pos++] = 0xC0 | c >> 6; out[pos++] = 0x80 | (c & 0x3F); } }
    else if (c < 0x10000) { if (pos + 3 < cap) { out[pos++] = 0xE0 | c >> 12; out[pos++] = 0x80 | (c >> 6 & 0x3F); out[pos++] = 0x80 | (c & 0x3F); } }
    else if (pos + 4 < cap) { out[pos++] = 0xF0 | c >> 18; out[pos++] = 0x80 | (c >> 12 & 0x3F); out[pos++] = 0x80 | (c >> 6 & 0x3F); out[pos++] = 0x80 | (c & 0x3F); }
    return pos;
}

// Converts an ID3 text payload (encoding byte already split off) to UTF-8.
static void decode_text(int enc, const u8 *p, int len, char *out, int cap)
{
    int o = 0;
    if (enc == 1 || enc == 2) {
        int big = enc == 2;
        if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) { big = 0; p += 2; len -= 2; }
        else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) { big = 1; p += 2; len -= 2; }
        for (int i = 0; i + 1 < len; i += 2) {
            u32 c = big ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
                u32 lo = big ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 3] << 8 | p[i + 2]);
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            o = put_utf8(out, o, cap, c);
        }
    } else {
        for (int i = 0; i < len && p[i]; i++) {
            if (enc != 3) o = put_utf8(out, o, cap, p[i]); // ISO-8859-1
            else if (o + 1 < cap) out[o++] = p[i];         // already UTF-8
        }
    }
    out[o] = 0;
    while (o > 0 && out[o - 1] == ' ') out[--o] = 0;
}

static void read_text_frame(SceUID fd, u32 size, char *out, int cap)
{
    u8 buf[512];
    int n = size < sizeof(buf) ? (int)size : (int)sizeof(buf);
    if (n < 1 || sceIoRead(fd, buf, n) != n) return;
    decode_text(buf[0], buf + 1, n - 1, out, cap);
}

static int number_prefix(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

// Locates the picture bytes inside an APIC (v2.3/2.4) or PIC (v2.2) frame.
static void read_picture(SceUID fd, u32 start, u32 size, int v22, TrackInfo *t)
{
    u8 head[256];
    int n = size < sizeof(head) ? (int)size : (int)sizeof(head);
    if (n < 4 || sceIoRead(fd, head, n) != n) return;
    int enc = head[0], i = 1;
    if (v22) i += 3;
    else { while (i < n && head[i]) i++; i++; } // MIME type
    if (i >= n) return;
    int type = head[i++];
    if (enc == 1 || enc == 2) { while (i + 1 < n && (head[i] || head[i + 1])) i += 2; i += 2; }
    else { while (i < n && head[i]) i++; i++; }
    if (i + 4 > n) return;
    int kind = head[i] == 0xFF && head[i + 1] == 0xD8 ? ART_JPEG : (head[i] == 0x89 && head[i + 1] == 'P') ? ART_PNG : ART_NONE;
    if (kind == ART_NONE) return;
    // Prefer the front cover (type 3); otherwise keep the first picture.
    if (t->art_type != ART_NONE && type != 3) return;
    t->art_offset = start + i;
    t->art_size = size - i;
    t->art_type = kind;
}

static u32 parse_id3v2(SceUID fd, TrackInfo *t)
{
    u8 h[10];
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    if (sceIoRead(fd, h, 10) != 10 || memcmp(h, "ID3", 3)) return 0;
    int ver = h[3], flags = h[5];
    u32 tag_end = 10 + syncsafe(h + 6) + (flags & 0x10 ? 10 : 0);
    if (ver < 2 || ver > 4) return tag_end;
    u32 pos = 10;
    if (flags & 0x40 && ver >= 3) {
        u8 e[4];
        sceIoRead(fd, e, 4);
        pos += ver == 4 ? syncsafe(e) : be32(e) + 4;
    }
    int v22 = ver == 2, hsize = v22 ? 6 : 10;
    while (pos + hsize <= tag_end) {
        u8 fh[10];
        sceIoLseek32(fd, pos, PSP_SEEK_SET);
        if (sceIoRead(fd, fh, hsize) != hsize || !fh[0]) break;
        u32 size = v22 ? (u32)(fh[3] << 16 | fh[4] << 8 | fh[5]) : ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        u32 body = pos + hsize;
        if (body + size > tag_end) break;
        char id[5] = { 0 };
        memcpy(id, fh, v22 ? 3 : 4);
        char num[16] = { 0 };
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) read_text_frame(fd, size, t->title, sizeof(t->title));
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) read_text_frame(fd, size, t->artist, sizeof(t->artist));
        else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) read_text_frame(fd, size, t->album_artist, sizeof(t->album_artist));
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) read_text_frame(fd, size, t->album, sizeof(t->album));
        else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) { read_text_frame(fd, size, num, sizeof(num)); t->track = number_prefix(num); }
        else if (!strcmp(id, "TPOS") || !strcmp(id, "TPA")) { read_text_frame(fd, size, num, sizeof(num)); t->disc = number_prefix(num); }
        else if (!strcmp(id, "APIC") || !strcmp(id, "PIC")) read_picture(fd, body, size, v22, t);
        pos = body + size;
    }
    return tag_end;
}

static void parse_id3v1(SceUID fd, u32 file_size, TrackInfo *t)
{
    u8 b[128];
    if (file_size < 128) return;
    sceIoLseek32(fd, file_size - 128, PSP_SEEK_SET);
    if (sceIoRead(fd, b, 128) != 128 || memcmp(b, "TAG", 3)) return;
    t->audio_end = file_size - 128;
    if (!t->title[0]) decode_text(0, b + 3, 30, t->title, sizeof(t->title));
    if (!t->artist[0]) decode_text(0, b + 33, 30, t->artist, sizeof(t->artist));
    if (!t->album[0]) decode_text(0, b + 63, 30, t->album, sizeof(t->album));
    if (!t->track && b[125] == 0 && b[126]) t->track = b[126];
}

static const int rates[3][3] = { { 44100, 48000, 32000 }, { 22050, 24000, 16000 }, { 11025, 12000, 8000 } };
static const int kbps[2][15] = {
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
};

// Finds the first Layer III frame at or after start and reads stream info.
static int parse_stream(SceUID fd, u32 start, TrackInfo *t)
{
    static u8 buf[4096];
    for (u32 base = start; base < start + 65536 && base < t->audio_end; base += sizeof(buf) - 4) {
        sceIoLseek32(fd, base, PSP_SEEK_SET);
        int n = sceIoRead(fd, buf, sizeof(buf));
        if (n < 4) return -1;
        for (int i = 0; i + 4 <= n; i++) {
            if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
            int ver = (buf[i + 1] >> 3) & 3, layer = (buf[i + 1] >> 1) & 3;
            int bri = buf[i + 2] >> 4, sri = (buf[i + 2] >> 2) & 3, pad = (buf[i + 2] >> 1) & 1;
            int mono = (buf[i + 3] >> 6) == 3;
            if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) continue;
            int mpeg1 = ver == 3, vi = ver == 3 ? 0 : ver == 2 ? 1 : 2;
            int rate = rates[vi][sri], spf = mpeg1 ? 1152 : 576;
            int bitrate = kbps[mpeg1 ? 0 : 1][bri] * 1000;
            u32 frame_len = (mpeg1 ? 144 : 72) * bitrate / rate + pad;
            u32 frame_pos = base + i;
            t->sample_rate = rate;
            t->audio_start = frame_pos;

            // Read the whole first frame to look for a Xing/Info or VBRI header.
            u8 fr[2048];
            sceIoLseek32(fd, frame_pos, PSP_SEEK_SET);
            int got = sceIoRead(fd, fr, frame_len < sizeof(fr) ? frame_len : sizeof(fr));
            int side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
            int x = 4 + side;
            u32 frames = 0;
            if (got >= x + 8 && (!memcmp(fr + x, "Xing", 4) || !memcmp(fr + x, "Info", 4))) {
                u32 flags = be32(fr + x + 4);
                int o = x + 8;
                if (flags & 1) { frames = be32(fr + o); o += 4; }
                if (flags & 2) o += 4;
                if (flags & 4) o += 100;
                if (flags & 8) o += 4;
                // LAME extension: 9-byte encoder string, then delay/padding at +21.
                if (o + 24 <= got && (!memcmp(fr + o, "LAME", 4) || !memcmp(fr + o, "Lavc", 4) || !memcmp(fr + o, "Lavf", 4))) {
                    t->enc_delay = fr[o + 21] << 4 | fr[o + 22] >> 4;
                    t->enc_padding = (fr[o + 22] & 0x0F) << 8 | fr[o + 23];
                }
                t->audio_start = frame_pos + frame_len; // the Info frame holds no audio
            } else if (got >= 36 + 18 && !memcmp(fr + 36, "VBRI", 4)) {
                frames = be32(fr + 36 + 14);
            }
            if (frames) {
                u64 samples = (u64)frames * spf;
                if (t->enc_delay >= 0) {
                    s64 exact = (s64)samples - t->enc_delay - t->enc_padding;
                    if (exact > 0) t->total_samples = (u32)exact;
                }
                t->duration_ms = (u32)((t->total_samples ? t->total_samples : samples) * 1000 / rate);
            } else if (bitrate) {
                t->duration_ms = (u32)((u64)(t->audio_end - frame_pos) * 8000 / bitrate);
            }
            return 0;
        }
    }
    return -1;
}

int tags_read(const char *path, TrackInfo *t)
{
    memset(t, 0, sizeof(*t));
    t->enc_delay = -1;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    u32 size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    t->audio_end = size;
    u32 start = parse_id3v2(fd, t);
    parse_id3v1(fd, size, t);
    int r = parse_stream(fd, start, t);
    sceIoClose(fd);
    return r;
}
