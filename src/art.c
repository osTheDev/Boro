#include "art.h"

#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "library.h"
#include "tags.h"

#define SLOTS 64
#define QUEUE 24

static const int kind_size[ART_KINDS] = { 72, 150, 72 };
static const float kind_radius[ART_KINDS] = { 4, 6, 36 };
static const int kind_limit[ART_KINDS] = { 40, 2, 16 };

typedef struct {
    int song, kind;   // song == -1: free
    Texture *tex;     // NULL while decoding or when decoding failed
    int failed;
    u32 color;
    u32 last_used;
} Slot;

static Slot slots[SLOTS];
static u32 frame_no;

// Requests go to the worker through a small stack (newest first), results
// come back through done[].
typedef struct { int song, kind; Texture *tex; u32 color; int ok; } Job;
static Job queue[QUEUE];
static volatile int queue_len;
static Job done[QUEUE];
static volatile int done_len;
static SceUID lock, wake;

static void take(void) { sceKernelWaitSema(lock, 1, NULL); }
static void give(void) { sceKernelSignalSema(lock, 1); }

// ---- Decoding -----------------------------------------------------------

typedef struct { struct jpeg_error_mgr mgr; jmp_buf jump; } JpegErr;

static void jpeg_fail(j_common_ptr cinfo)
{
    longjmp(((JpegErr *)cinfo->err)->jump, 1);
}

// Decodes to tightly packed RGBA. Large JPEGs are scaled down while decoding.
static u8 *decode_jpeg(const u8 *data, u32 size, int want, int *w, int *h)
{
    struct jpeg_decompress_struct cinfo;
    JpegErr err;
    u8 *volatile out = NULL;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        free(out);
        return NULL;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)data, size);
    jpeg_read_header(&cinfo, TRUE);
    int small = cinfo.image_width < cinfo.image_height ? cinfo.image_width : cinfo.image_height;
    int denom = 8;
    while (denom > 1 && small / denom < want) denom /= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    *w = cinfo.output_width;
    *h = cinfo.output_height;
    out = malloc(*w * *h * 4);
    u8 *row = malloc(*w * 3);
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

static u8 *decode_png(const u8 *data, u32 size, int *w, int *h)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, data, size)) return NULL;
    img.format = PNG_FORMAT_RGBA;
    u8 *out = malloc(PNG_IMAGE_SIZE(img));
    if (!out || !png_image_finish_read(&img, NULL, out, 0, NULL)) {
        png_image_free(&img);
        free(out);
        return NULL;
    }
    *w = img.width;
    *h = img.height;
    return out;
}

// Center-crops to a square, box-filters down to size and applies the
// rounded-corner mask. Also returns the average color.
static Texture *make_texture(const u8 *rgba, int w, int h, int size, float radius, u32 *avg)
{
    Texture *t = tex_create(size, size);
    int side = w < h ? w : h, ox = (w - side) / 2, oy = (h - side) / 2;
    float scale = (float)side / size;
    u32 sr = 0, sg = 0, sb = 0;
    for (int y = 0; y < size; y++) {
        int y0 = oy + (int)(y * scale), y1 = oy + (int)((y + 1) * scale);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < size; x++) {
            int x0 = ox + (int)(x * scale), x1 = ox + (int)((x + 1) * scale);
            if (x1 <= x0) x1 = x0 + 1;
            u32 r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) {
                    const u8 *p = rgba + (yy * w + xx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            if (!n) n = 1;
            r /= n; g /= n; b /= n;
            sr += r; sg += g; sb += b;
            // Rounded corner coverage.
            float cx = x + 0.5f, cy = y + 0.5f, a = 1;
            float qx = fabsf(cx - size * 0.5f) - (size * 0.5f - radius), qy = fabsf(cy - size * 0.5f) - (size * 0.5f - radius);
            if (qx > 0 && qy > 0) {
                float d = sqrtf(qx * qx + qy * qy) - radius;
                a = d < -0.5f ? 1 : d > 0.5f ? 0 : 0.5f - d;
            } else if (qx > radius || qy > radius) {
                a = 0;
            }
            t->px[y * t->tw + x] = RGBA(r, g, b, (int)(a * 255));
        }
    }
    u32 n = size * size;
    *avg = RGBA(sr / n, sg / n, sb / n, 255);
    tex_upload(t);
    return t;
}

// Image files (playlist covers) use keys above every song id.
#define FILE_KEY 0x1000000
#define FILES 8
static char file_paths[FILES][320];
static int file_next;

static u8 *read_file(const char *path, u32 *size)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    int n = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    u8 *data = n > 0 && n <= 8 * 1024 * 1024 ? malloc(n) : NULL;
    if (data && sceIoRead(fd, data, n) != n) { free(data); data = NULL; }
    sceIoClose(fd);
    *size = n;
    return data;
}

static int decode_file(Job *j)
{
    char path[320];
    take();
    snprintf(path, sizeof(path), "%s", file_paths[j->song - FILE_KEY]);
    give();
    u32 size;
    u8 *data = read_file(path, &size);
    if (!data) return 0;
    int w = 0, h = 0, png = size > 4 && data[0] == 0x89 && data[1] == 'P';
    u8 *rgba = png ? decode_png(data, size, &w, &h) : decode_jpeg(data, size, kind_size[j->kind], &w, &h);
    free(data);
    if (!rgba) return 0;
    j->tex = make_texture(rgba, w, h, kind_size[j->kind], kind_radius[j->kind], &j->color);
    free(rgba);
    return 1;
}

static int decode(Job *j)
{
    if (j->song >= FILE_KEY) return decode_file(j);
    const Song *s = &lib.songs[j->song];
    if (!s->art_type || !s->art_size || s->art_size > 8 * 1024 * 1024) return 0;
    SceUID fd = sceIoOpen(song_path(s), PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    u8 *data = malloc(s->art_size);
    int ok = data && sceIoLseek32(fd, s->art_offset, PSP_SEEK_SET) == (int)s->art_offset &&
             sceIoRead(fd, data, s->art_size) == (int)s->art_size;
    sceIoClose(fd);
    int w = 0, h = 0;
    u8 *rgba = NULL;
    if (ok) rgba = s->art_type == ART_JPEG ? decode_jpeg(data, s->art_size, kind_size[j->kind], &w, &h)
                                           : decode_png(data, s->art_size, &w, &h);
    free(data);
    if (!rgba) return 0;
    j->tex = make_texture(rgba, w, h, kind_size[j->kind], kind_radius[j->kind], &j->color);
    free(rgba);
    return 1;
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(wake, 1, NULL);
        take();
        if (!queue_len) { give(); continue; }
        Job j = queue[--queue_len];
        give();
        j.ok = decode(&j);
        take();
        if (done_len < QUEUE) done[done_len++] = j;
        else tex_destroy(j.tex);
        give();
    }
    return 0;
}

// ---- Cache ----------------------------------------------------------------

void art_init(void)
{
    for (int i = 0; i < SLOTS; i++) slots[i].song = -1;
    lock = sceKernelCreateSema("art_lock", 0, 1, 1, NULL);
    wake = sceKernelCreateSema("art_wake", 0, 0, 1000, NULL);
    SceUID th = sceKernelCreateThread("boro_art", worker, 0x38, 0x20000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

static Slot *find(int song, int kind)
{
    for (int i = 0; i < SLOTS; i++)
        if (slots[i].song == song && slots[i].kind == kind) return &slots[i];
    return NULL;
}

static Slot *claim(int kind)
{
    // Free slot, else the least recently used slot of the same kind once that
    // kind is at its limit, else the least recently used finished slot.
    int used = 0;
    Slot *free_slot = NULL, *lru_kind = NULL, *lru_any = NULL;
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &slots[i];
        if (s->song < 0) { if (!free_slot) free_slot = s; continue; }
        int busy = !s->tex && !s->failed;
        if (s->kind == kind) {
            used++;
            if (!busy && (!lru_kind || s->last_used < lru_kind->last_used)) lru_kind = s;
        }
        if (!busy && s->last_used != frame_no && (!lru_any || s->last_used < lru_any->last_used)) lru_any = s;
    }
    Slot *s = used >= kind_limit[kind] && lru_kind ? lru_kind : free_slot ? free_slot : lru_any;
    if (!s) return NULL;
    tex_destroy(s->tex);
    memset(s, 0, sizeof(*s));
    s->song = -1;
    return s;
}

static const Texture *get(int song, int kind)
{
    Slot *s = find(song, kind);
    if (!s) {
        s = claim(kind);
        if (!s) return NULL;
        s->song = song;
        s->kind = kind;
        take();
        if (queue_len == QUEUE) memmove(queue, queue + 1, --queue_len * sizeof(Job));
        queue[queue_len++] = (Job){ song, kind, NULL, 0, 0 };
        give();
        sceKernelSignalSema(wake, 1);
    }
    s->last_used = frame_no;
    return s->tex;
}

const Texture *art_get(int song, int kind)
{
    if (song < 0 || song >= lib.song_count || !lib.songs[song].art_type) return NULL;
    return get(song, kind);
}

const Texture *art_get_file(const char *path, int kind)
{
    int i = 0;
    while (i < FILES && strcmp(file_paths[i], path)) i++;
    if (i == FILES) {
        // Reuse the oldest path; its slots are dropped so they can't show
        // the wrong picture.
        i = file_next;
        file_next = (file_next + 1) % FILES;
        for (int k = 0; k < SLOTS; k++)
            if (slots[k].song == FILE_KEY + i && (slots[k].tex || slots[k].failed)) {
                tex_destroy(slots[k].tex);
                slots[k].tex = NULL;
                slots[k].song = -1;
            }
        take();
        snprintf(file_paths[i], sizeof(file_paths[i]), "%s", path);
        give();
    }
    return get(FILE_KEY + i, kind);
}

u32 art_color(int song, u32 fallback)
{
    for (int k = 0; k < ART_KINDS; k++) {
        Slot *s = find(song, k);
        if (s && s->tex) return s->color;
    }
    return fallback;
}

void art_frame(void)
{
    frame_no++;
    take();
    for (int i = 0; i < done_len; i++) {
        Slot *s = find(done[i].song, done[i].kind);
        if (!s) { tex_destroy(done[i].tex); continue; }
        s->tex = done[i].tex;
        s->color = done[i].color;
        s->failed = !done[i].ok;
    }
    done_len = 0;
    // Requests that fell off the queue would wait forever: retry them.
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &slots[i];
        if (s->song < 0 || s->tex || s->failed || frame_no - s->last_used < 2) continue;
        int queued = 0;
        for (int q = 0; q < queue_len; q++)
            if (queue[q].song == s->song && queue[q].kind == s->kind) queued = 1;
        if (!queued) s->song = -1;
    }
    give();
}
