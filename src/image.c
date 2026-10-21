#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <jerror.h>
#include <png.h>
#include <pspiofilemgr.h>
#include <setjmp.h>

#define MAX_PIXELS (3 * 1024 * 1024) // 12 MB of RGBA
#define RESERVE (2 * 1024 * 1024)     // left for the rest of Boro while decoding

// The biggest single block malloc can give right now. Other threads keep
// everything else while this looks.
static u32 heap_largest(void)
{
    u32 lo = 0, hi = 64u << 20;
    while (hi - lo > (64 << 10)) {
        u32 m = (lo + hi) / 2;
        void *volatile b = malloc(m);
        if (b) { free(b); lo = m; } else hi = m;
    }
    return lo;
}

typedef struct { struct jpeg_error_mgr mgr; jmp_buf jump; } JpegErr;

static void jpeg_fail(j_common_ptr cinfo)
{
    longjmp(((JpegErr *)cinfo->err)->jump, 1);
}

static void jpeg_quiet(j_common_ptr cinfo) { (void)cinfo; }

// Reads from memory when data is set, else from the file. Sets *w to -1 when
// the picture is fine but too big for the memory left.
static u8 *decode_jpeg(const u8 *data, u32 size, FILE *file, int want, int *w, int *h)
{
    volatile int too_big = 0;
    struct jpeg_decompress_struct cinfo;
    JpegErr err;
    u8 *volatile out = NULL;
    u8 *volatile row = NULL;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    err.mgr.output_message = jpeg_quiet;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        free(out);
        free(row);
        *w = too_big || err.mgr.msg_code == JERR_OUT_OF_MEMORY ? -1 : 0;
        return NULL;
    }
    jpeg_create_decompress(&cinfo);
    if (data) jpeg_mem_src(&cinfo, (unsigned char *)data, size);
    else jpeg_stdio_src(&cinfo, file);
    jpeg_read_header(&cinfo, TRUE);
    int small = cinfo.image_width < cinfo.image_height ? cinfo.image_width : cinfo.image_height;
    int denom = 8;
    if (want <= 0) denom = 1;
    while (denom > 1 && small / denom < want) denom /= 2;
    // Never more than fits in memory, whatever was asked for.
    while (denom < 8 && (cinfo.image_width / denom) * (cinfo.image_height / denom) > MAX_PIXELS) denom *= 2;
    // Progressive JPEGs hold every coefficient of the full-size picture while
    // decoding (36 MB for a 12 MP photo), however small the output. The output
    // is counted twice: callers make one more copy (turned upright, or tiled).
    u32 coef = 0;
    if (cinfo.progressive_mode)
        for (int c = 0; c < cinfo.num_components; c++)
            coef += cinfo.comp_info[c].width_in_blocks * cinfo.comp_info[c].height_in_blocks * sizeof(JBLOCK);
    u32 room = heap_largest();
#define NEED(d) ((cinfo.image_width / (d) + 1) * (cinfo.image_height / (d) + 1) * 8 + coef + RESERVE)
    while (denom < 8 && NEED(denom) > room) denom *= 2;
    if (NEED(denom) > room) {
        too_big = 1;
        longjmp(err.jump, 1);
    }
#undef NEED
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    *w = cinfo.output_width;
    *h = cinfo.output_height;
    out = *w * *h <= MAX_PIXELS ? malloc(*w * *h * 4) : NULL;
    row = malloc(*w * 3);
    if (!out || !row) {
        too_big = 1;
        longjmp(err.jump, 1);
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        u8 *rows[1] = { row };
        int y = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, rows, 1);
        for (int x = 0; x < *w; x++) {
            u8 *d = out + (y * *w + x) * 4;
            d[0] = row[x * 3];
            d[1] = row[x * 3 + 1];
            d[2] = row[x * 3 + 2];
            d[3] = 255;
        }
    }
    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return out;
}

// png_image has begin_read calls for memory and for files.
static u8 *finish_png(png_image *img, int *w, int *h)
{
    img->format = PNG_FORMAT_RGBA;
    u8 *out = img->width * img->height <= MAX_PIXELS ? malloc(PNG_IMAGE_SIZE(*img)) : NULL;
    if (!out || !png_image_finish_read(img, NULL, out, 0, NULL)) {
        png_image_free(img);
        *w = out ? 0 : -1;
        free(out);
        return NULL;
    }
    *w = img->width;
    *h = img->height;
    return out;
}

static int is_png(const u8 *data, u32 size) { return size > 4 && data[0] == 0x89 && data[1] == 'P'; }

u8 *image_decode(const u8 *data, u32 size, int want, int *w, int *h)
{
    if (!is_png(data, size)) return decode_jpeg(data, size, NULL, want, w, h);
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, data, size)) return NULL;
    return finish_png(&img, w, h);
}

// ---- EXIF orientation -------------------------------------------------------

static u32 get16(const u8 *p, int le) { return le ? p[0] | p[1] << 8 : p[0] << 8 | p[1]; }
static u32 get32(const u8 *p, int le) { return le ? get16(p, 1) | get16(p + 2, 1) << 16 : get16(p, 0) << 16 | get16(p + 2, 0); }

// What the APP1 Exif block near the start of a JPEG says.
typedef struct {
    int orient;              // 1 (upright) to 8
    const u8 *thumb;         // the camera's own small JPEG preview, if it is in reach
    u32 thumb_size;
    u32 width, height;       // of the full picture, 0 if not given
} Exif;

// Reads one IFD's entries; returns the offset of the next IFD (0 if none).
static u32 read_ifd(const u8 *tiff, u32 tn, int le, u32 ifd, Exif *e, u32 *sub)
{
    if (ifd + 2 > tn) return 0;
    u32 count = get16(tiff + ifd, le), thumb_at = 0;
    for (u32 k = 0; k < count; k++) {
        const u8 *t = tiff + ifd + 2 + k * 12;
        if (ifd + 2 + k * 12 + 12 > tn) return 0;
        u32 tag = get16(t, le), type = get16(t + 2, le);
        u32 v = type == 3 ? get16(t + 8, le) : get32(t + 8, le);
        switch (tag) {
        case 0x0112: e->orient = v >= 1 && v <= 8 ? v : 1; break;
        case 0x8769: *sub = v; break;
        case 0xA002: e->width = v; break;
        case 0xA003: e->height = v; break;
        case 0x0201: thumb_at = v; break;
        case 0x0202:
            if (thumb_at && thumb_at < tn && v <= tn - thumb_at) { e->thumb = tiff + thumb_at; e->thumb_size = v; }
            break;
        }
    }
    u32 at = ifd + 2 + count * 12;
    return at + 4 <= tn ? get32(tiff + at, le) : 0;
}

static void exif_read(const u8 *d, u32 n, Exif *e)
{
    memset(e, 0, sizeof(*e));
    e->orient = 1;
    u32 i = 2;
    while (i + 4 <= n && d[i] == 0xFF) {
        int marker = d[i + 1];
        u32 len = get16(d + i + 2, 0);
        if (marker == 0xDA || len < 2) break; // image data starts
        const u8 *seg = d + i + 4;
        if (marker == 0xE1 && len > 14 && i + 18 <= n && !memcmp(seg, "Exif\0\0", 6)) {
            const u8 *tiff = seg + 6;
            // A block running past what was read is still read as far as it goes.
            u32 tn = (i + 2 + len <= n ? i + 2 + len : n) - (i + 10);
            int le = tiff[0] == 'I';
            u32 sub = 0, next = read_ifd(tiff, tn, le, get32(tiff + 4, le), e, &sub);
            if (sub) read_ifd(tiff, tn, le, sub, e, &sub);
            if (next) read_ifd(tiff, tn, le, next, e, &sub); // IFD1: the thumbnail
            return;
        }
        i += 2 + len;
    }
}

// Returns a new buffer turned to match the orientation, freeing the old one.
static u8 *orient(u8 *src, int *w, int *h, int o)
{
    if (o <= 1) return src;
    int sw = *w, sh = *h, swap = o >= 5;
    int dw = swap ? sh : sw, dh = swap ? sw : sh;
    u32 *out = malloc(dw * dh * 4);
    if (!out) return src;
    const u32 *s = (const u32 *)src;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            int sx, sy;
            switch (o) {
            case 2: sx = sw - 1 - x; sy = y; break;
            case 3: sx = sw - 1 - x; sy = sh - 1 - y; break;
            case 4: sx = x; sy = sh - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = sh - 1 - x; break;
            case 7: sx = sw - 1 - y; sy = sh - 1 - x; break;
            default: sx = sw - 1 - y; sy = x; break;
            }
            out[y * dw + x] = s[sy * sw + sx];
        }
    free(src);
    *w = dw;
    *h = dh;
    return (u8 *)out;
}

u8 *image_load(const char *path, int want, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    u8 *head = malloc(64 * 1024);
    u32 n = head ? fread(head, 1, 64 * 1024, f) : 0;
    u8 *out = NULL;
    int png = is_png(head, n), jpeg = n >= 4 && head[0] == 0xFF && head[1] == 0xD8;
    Exif exif;
    exif_read(head, jpeg ? n : 0, &exif);
    // A small picture can come from the camera's own preview, which is far
    // quicker than reading a whole 12 MP photo. Only when it is big enough and
    // has the photo's shape (some are letterboxed to 4:3).
    if (jpeg && want > 0 && exif.thumb && exif.width && exif.height) {
        u8 *small = image_decode(exif.thumb, exif.thumb_size, want, w, h);
        if (small && (*w < *h ? *w : *h) >= want && abs((int)(*w * exif.height) - (int)(*h * exif.width)) * 50 <= (int)(*w * exif.height)) {
            free(head);
            fclose(f);
            return orient(small, w, h, exif.orient);
        }
        free(small);
    }
    int o = exif.orient;
    free(head);
    if (png) {
        fclose(f);
        png_image img;
        memset(&img, 0, sizeof(img));
        img.version = PNG_IMAGE_VERSION;
        if (png_image_begin_read_from_file(&img, path)) out = finish_png(&img, w, h);
        return out;
    }
    if (!jpeg) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    out = decode_jpeg(NULL, 0, f, want, w, h);
    fclose(f);
    return out ? orient(out, w, h, o) : NULL;
}

u8 *image_read_file(const char *path, u32 max, u32 *size)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    int n = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    u8 *data = n > 0 && (u32)n <= max ? malloc(n) : NULL;
    if (data && sceIoRead(fd, data, n) != n) { free(data); data = NULL; }
    sceIoClose(fd);
    *size = n;
    return data;
}
