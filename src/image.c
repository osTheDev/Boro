#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>
#include <png.h>
#include <pspiofilemgr.h>
#include <setjmp.h>

#define MAX_PIXELS (3 * 1024 * 1024) // 12 MB of RGBA

typedef struct { struct jpeg_error_mgr mgr; jmp_buf jump; } JpegErr;

static void jpeg_fail(j_common_ptr cinfo)
{
    longjmp(((JpegErr *)cinfo->err)->jump, 1);
}

static void jpeg_quiet(j_common_ptr cinfo) { (void)cinfo; }

// Reads from memory when data is set, else from the file.
static u8 *decode_jpeg(const u8 *data, u32 size, FILE *file, int want, int *w, int *h)
{
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
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    *w = cinfo.output_width;
    *h = cinfo.output_height;
    out = *w * *h <= MAX_PIXELS ? malloc(*w * *h * 4) : NULL;
    row = malloc(*w * 3);
    if (!out || !row) longjmp(err.jump, 1);
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

// 1 (upright) to 8, from the APP1 Exif block near the start of a JPEG.
static int exif_orientation(const u8 *d, u32 n)
{
    u32 i = 2;
    while (i + 4 <= n && d[i] == 0xFF) {
        int marker = d[i + 1];
        u32 len = get16(d + i + 2, 0);
        if (marker == 0xDA || len < 2) break; // image data starts
        const u8 *seg = d + i + 4;
        if (marker == 0xE1 && i + 2 + len <= n && len > 14 && !memcmp(seg, "Exif\0\0", 6)) {
            const u8 *tiff = seg + 6;
            u32 tn = len - 8;
            int le = tiff[0] == 'I';
            u32 ifd = get32(tiff + 4, le);
            if (ifd + 2 > tn) return 1;
            u32 count = get16(tiff + ifd, le);
            for (u32 k = 0; k < count && ifd + 2 + k * 12 + 12 <= tn; k++) {
                const u8 *e = tiff + ifd + 2 + k * 12;
                if (get16(e, le) == 0x0112) {
                    int o = get16(e + 8, le);
                    return o >= 1 && o <= 8 ? o : 1;
                }
            }
            return 1;
        }
        i += 2 + len;
    }
    return 1;
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
    int o = jpeg ? exif_orientation(head, n) : 1;
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
