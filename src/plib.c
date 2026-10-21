#include "plib.h"

#include <math.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "image.h"

#define THUMB_MAGIC 0x31485450 // "PTH1"
#define LOADED 24              // thumbnail textures kept in memory
#define WANTED 16

Photo *photos;
int photo_count, loose_first;
PhotoAlbum *palbums;
int palbum_count;

static int photo_cap, palbum_cap;
static char data_dir[256], picture_dir[256];
static SceUID busy = -1;
static volatile int thumbs_on;
static volatile int wanted[WANTED];
static volatile int wanted_next;

typedef struct { int index; Texture *tex; u32 used; } Loaded;
static Loaded loaded[LOADED];
static u32 use_clock;

typedef struct { u32 magic, modified; s32 has_pixels; } ThumbHead;

static u32 fnv(const char *s)
{
    u32 h = 2166136261u;
    while (*s) h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

static void thumb_path(const Photo *p, char *out, int size)
{
    snprintf(out, size, "%s/pthumbs/%08X.bin", data_dir, (unsigned)fnv(p->path));
}

// ---- Scan ---------------------------------------------------------------

static int is_picture(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png"));
}

static void add_photo(const char *path, const char *name, ScePspDateTime *mtime)
{
    if (strlen(path) >= sizeof(photos->path)) return;
    if (photo_count == photo_cap) {
        photo_cap = photo_cap ? photo_cap * 2 : 64;
        photos = realloc(photos, photo_cap * sizeof(Photo));
    }
    Photo *p = &photos[photo_count++];
    memset(p, 0, sizeof(*p));
    strcpy(p->path, path);
    snprintf(p->name, sizeof(p->name), "%.63s", name);
    u64 tick = 0;
    sceRtcGetTick(mtime, &tick);
    p->modified = (u32)(tick / 1000000 - 63000000000ULL);
}

// At the top (depth 0) folders become palbums and pictures are left for
// add_loose; inside an album every picture is added, subfolders included.
static void walk(const char *dir, int depth)
{
    if (depth > 4) return;
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent ent;
    char path[512];
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (ent.d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            if (depth > 0) {
                walk(path, depth + 1);
            } else {
                if (palbum_count == palbum_cap) {
                    palbum_cap = palbum_cap ? palbum_cap * 2 : 16;
                    palbums = realloc(palbums, palbum_cap * sizeof(PhotoAlbum));
                }
                PhotoAlbum *a = &palbums[palbum_count++];
                memset(a, 0, sizeof(*a));
                snprintf(a->name, sizeof(a->name), "%.63s", ent.d_name);
            }
        } else if (depth > 0 && is_picture(ent.d_name)) {
            add_photo(path, ent.d_name, &ent.d_stat.sce_st_mtime);
        }
    }
    sceIoDclose(d);
}

static void add_loose(void)
{
    SceUID d = sceIoDopen(picture_dir);
    if (d < 0) return;
    SceIoDirent ent;
    char path[512];
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (ent.d_name[0] == '.' || FIO_S_ISDIR(ent.d_stat.st_mode) || !is_picture(ent.d_name)) continue;
        snprintf(path, sizeof(path), "%s/%s", picture_dir, ent.d_name);
        add_photo(path, ent.d_name, &ent.d_stat.sce_st_mtime);
    }
    sceIoDclose(d);
}

static int cmp_album(const void *a, const void *b) { return strcasecmp(((const PhotoAlbum *)a)->name, ((const PhotoAlbum *)b)->name); }
static int cmp_photo(const void *a, const void *b) { return strcasecmp(((const Photo *)a)->name, ((const Photo *)b)->name); }

static void read_thumb_head(Photo *p)
{
    char path[300];
    thumb_path(p, path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    ThumbHead h;
    if (sceIoRead(fd, &h, sizeof(h)) == sizeof(h) && h.magic == THUMB_MAGIC && h.modified == p->modified)
        p->thumb = h.has_pixels ? 1 : -1;
    sceIoClose(fd);
}

void plib_scan(void)
{
    plib_thumbs(0);
    for (int i = 0; i < LOADED; i++) loaded[i].index = -1;
    photo_count = palbum_count = 0;
    walk(picture_dir, 0);
    qsort(palbums, palbum_count, sizeof(PhotoAlbum), cmp_album);
    int kept = 0;
    char path[512];
    for (int i = 0; i < palbum_count; i++) {
        PhotoAlbum a = palbums[i];
        a.first = photo_count;
        snprintf(path, sizeof(path), "%s/%s", picture_dir, a.name);
        walk(path, 1);
        a.count = photo_count - a.first;
        qsort(photos + a.first, a.count, sizeof(Photo), cmp_photo);
        if (a.count) palbums[kept++] = a; // empty folders aren't palbums
    }
    palbum_count = kept;
    loose_first = photo_count;
    add_loose();
    qsort(photos + loose_first, photo_count - loose_first, sizeof(Photo), cmp_photo);
    for (int i = 0; i < photo_count; i++) read_thumb_head(&photos[i]);
    for (int i = 0; i < WANTED; i++) wanted[i] = -1;
}

// ---- Thumbnails -----------------------------------------------------------

// Crops to the thumbnail's shape around the middle and box-filters down.
static void shrink(const u8 *rgba, int w, int h, u32 *out)
{
    int cw = w, ch = h, ox = 0, oy = 0;
    if (w * PTHUMB_H > h * PTHUMB_W) { cw = h * PTHUMB_W / PTHUMB_H; ox = (w - cw) / 2; }
    else { ch = w * PTHUMB_H / PTHUMB_W; oy = (h - ch) / 2; }
    for (int y = 0; y < PTHUMB_H; y++) {
        int y0 = oy + y * ch / PTHUMB_H, y1 = oy + (y + 1) * ch / PTHUMB_H;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < PTHUMB_W; x++) {
            int x0 = ox + x * cw / PTHUMB_W, x1 = ox + (x + 1) * cw / PTHUMB_W;
            if (x1 <= x0) x1 = x0 + 1;
            u32 r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) {
                    const u8 *p = rgba + (yy * w + xx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            if (!n) n = 1;
            out[y * PTHUMB_W + x] = RGBA(r / n, g / n, b / n, 255);
        }
    }
}

static void make_thumb(Photo *p)
{
    static u32 px[PTHUMB_W * PTHUMB_H];
    ThumbHead h = { THUMB_MAGIC, p->modified, 0 };
    int w, hh;
    u8 *rgba = image_load(p->path, PTHUMB_H, &w, &hh);
    if (rgba) {
        shrink(rgba, w, hh, px);
        free(rgba);
        h.has_pixels = 1;
    }
    char path[300];
    thumb_path(p, path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, &h, sizeof(h));
        if (h.has_pixels) sceIoWrite(fd, px, sizeof(px));
        sceIoClose(fd);
    }
    p->thumb = h.has_pixels ? 1 : -1;
}

// Rounded corners go in the alpha, so a thumbnail can sit over anything.
static void round_alpha(Texture *t, float r)
{
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            float qx = fabsf(x + 0.5f - t->w * 0.5f) - (t->w * 0.5f - r);
            float qy = fabsf(y + 0.5f - t->h * 0.5f) - (t->h * 0.5f - r);
            if (qx <= 0 || qy <= 0) continue;
            float d = sqrtf(qx * qx + qy * qy) - r;
            float a = d < -0.5f ? 1 : d > 0.5f ? 0 : 0.5f - d;
            u32 *p = &t->px[y * t->tw + x];
            *p = (*p & 0xFFFFFF) | (u32)(a * 255) << 24;
        }
}

// What's on screen first, newest request first; then the rest in order.
static int next_thumb(void)
{
    for (int k = 0; k < WANTED; k++) {
        int i = wanted[(wanted_next + WANTED - 1 - k) % WANTED];
        if (i >= 0 && i < photo_count && !photos[i].thumb) return i;
    }
    for (int i = 0; i < photo_count; i++)
        if (!photos[i].thumb) return i;
    return -1;
}

static int worker_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        if (!thumbs_on) { sceKernelDelayThread(50000); continue; }
        sceKernelWaitSema(busy, 1, NULL);
        int i = thumbs_on ? next_thumb() : -1;
        if (i >= 0) make_thumb(&photos[i]);
        sceKernelSignalSema(busy, 1);
        sceKernelDelayThread(i < 0 ? 200000 : 1000);
    }
    return 0;
}

void plib_thumbs(int on)
{
    thumbs_on = on;
    if (!on && busy >= 0) {
        sceKernelWaitSema(busy, 1, NULL);
        sceKernelSignalSema(busy, 1);
    }
}

void plib_want(int index)
{
    if (index < 0 || index >= photo_count || photos[index].thumb) return;
    for (int k = 0; k < WANTED; k++) if (wanted[k] == index) return;
    wanted[wanted_next] = index;
    wanted_next = (wanted_next + 1) % WANTED;
}

const Texture *plib_thumb(int index)
{
    if (index < 0 || index >= photo_count || photos[index].thumb != 1) return NULL;
    use_clock++;
    int slot = 0;
    for (int i = 0; i < LOADED; i++) {
        if (loaded[i].index == index) { loaded[i].used = use_clock; return loaded[i].tex; }
        if (loaded[i].used < loaded[slot].used) slot = i;
    }
    Loaded *l = &loaded[slot];
    if (!l->tex) l->tex = tex_create(PTHUMB_W, PTHUMB_H);
    l->index = -1;
    char path[300];
    thumb_path(&photos[index], path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    ThumbHead h;
    int ok = sceIoRead(fd, &h, sizeof(h)) == sizeof(h) && h.has_pixels;
    for (int y = 0; ok && y < PTHUMB_H; y++)
        ok = sceIoRead(fd, l->tex->px + y * l->tex->tw, PTHUMB_W * 4) == PTHUMB_W * 4;
    sceIoClose(fd);
    if (!ok) { photos[index].thumb = 0; return NULL; }
    round_alpha(l->tex, 5);
    tex_upload(l->tex);
    l->index = index;
    l->used = use_clock;
    return l->tex;
}

const char *plib_dir(void) { return picture_dir; }

void plib_init(const char *data, const char *dir)
{
    snprintf(data_dir, sizeof(data_dir), "%s", data);
    snprintf(picture_dir, sizeof(picture_dir), "%s", dir);
    char path[300];
    snprintf(path, sizeof(path), "%s/pthumbs", data_dir);
    sceIoMkdir(path, 0777);
    for (int i = 0; i < LOADED; i++) loaded[i].index = -1;
    busy = sceKernelCreateSema("pthumbs", 0, 1, 1, NULL);
    SceUID th = sceKernelCreateThread("boro_pthumbs", worker_thread, 0x22, 0x20000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}
