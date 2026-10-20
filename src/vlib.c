#include "vlib.h"

#include <malloc.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "avc.h"
#include "mp4.h"

#define THUMB_W 160
#define THUMB_H 90
#define THUMB_MAGIC 0x31485442 // "BTH1"
#define LOADED 12              // thumbnail textures kept in memory

Video *videos;
int video_count;

static char data_dir[256], video_dir[256];
static SceUID busy = -1;
static volatile int thumbs_on;
static SceUID worker = -1;

typedef struct { int index; Texture *tex; u32 used; } Loaded;
static Loaded loaded[LOADED];
static u32 use_clock;

// Seconds since about 1997: fits a u32 for a long while.
static u32 tick_seconds(u64 tick) { return (u32)(tick / 1000000 - 63000000000ULL); }
static u32 now_seconds(void) { u64 t; sceRtcGetCurrentTick(&t); return tick_seconds(t); }

static u32 fnv(const char *s)
{
    u32 h = 2166136261u;
    while (*s) h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

static void thumb_path(const Video *v, char *out, int size)
{
    snprintf(out, size, "%s/thumbs/%08X.bin", data_dir, (unsigned)fnv(v->path));
}

typedef struct {
    u32 magic, modified, duration_ms;
    u16 width, height;
    s32 has_pixels;
} ThumbHead;

// ---- Resume -------------------------------------------------------------

static void resume_path(char *out, int size) { snprintf(out, size, "%s/video.txt", data_dir); }

static void load_resume(void)
{
    char path[300];
    resume_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[400];
    while (fgets(line, sizeof(line), f)) {
        char *a = strchr(line, '\t');
        if (!a) continue;
        *a++ = 0;
        unsigned pos = 0, when = 0;
        sscanf(a, "%u\t%u", &pos, &when);
        for (int i = 0; i < video_count; i++)
            if (!strcmp(videos[i].path, line)) { videos[i].resume_ms = pos; videos[i].watched = when; }
    }
    fclose(f);
}

static void save_resume(void)
{
    char path[300];
    resume_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < video_count; i++)
        if (videos[i].watched) fprintf(f, "%s\t%u\t%u\n", videos[i].path, (unsigned)videos[i].resume_ms, (unsigned)videos[i].watched);
    fclose(f);
}

void vlib_set_resume(int index, u32 ms)
{
    if (index < 0 || index >= video_count) return;
    Video *v = &videos[index];
    // The last half minute (or 5%) is credits: count it as finished.
    u32 d = v->duration_ms;
    if (d && (ms + 30000 >= d || ms >= d / 20 * 19)) ms = 0;
    v->resume_ms = ms;
    v->watched = now_seconds();
    save_resume();
}

int vlib_continue(void)
{
    int best = -1;
    for (int i = 0; i < video_count; i++)
        if (videos[i].resume_ms && (best < 0 || videos[i].watched > videos[best].watched)) best = i;
    return best;
}

// ---- Scan ---------------------------------------------------------------

static int is_mp4(const char *name)
{
    int n = strlen(name);
    return n > 4 && (!strcasecmp(name + n - 4, ".mp4") || !strcasecmp(name + n - 4, ".m4v"));
}

static void make_title(char *out, int size, const char *name)
{
    snprintf(out, size, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

static int category_of(const char *top)
{
    if (!strcasecmp(top, "Movies")) return VCAT_MOVIES;
    if (!strcasecmp(top, "Shows")) return VCAT_SHOWS;
    if (!strcasecmp(top, "Clips")) return VCAT_CLIPS;
    return VCAT_OTHER;
}

static int cap;

static void walk(const char *dir, int category, int depth)
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
            walk(path, depth == 0 ? category_of(ent.d_name) : category, depth + 1);
        } else if (is_mp4(ent.d_name) && strlen(path) < sizeof(videos->path)) {
            if (video_count == cap) {
                cap = cap ? cap * 2 : 32;
                videos = realloc(videos, cap * sizeof(Video));
            }
            Video *v = &videos[video_count++];
            memset(v, 0, sizeof(*v));
            strcpy(v->path, path);
            make_title(v->title, sizeof(v->title), ent.d_name);
            v->category = category;
            u64 tick = 0;
            sceRtcGetTick(&ent.d_stat.sce_st_mtime, &tick);
            v->modified = tick_seconds(tick);
        }
    }
    sceIoDclose(d);
}

// Fills duration, size and whether a thumbnail exists from the cache file.
static void read_thumb_head(Video *v)
{
    char path[300];
    thumb_path(v, path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    ThumbHead h;
    if (sceIoRead(fd, &h, sizeof(h)) == sizeof(h) && h.magic == THUMB_MAGIC && h.modified == v->modified) {
        v->duration_ms = h.duration_ms;
        v->width = h.width;
        v->height = h.height;
        v->thumb = h.has_pixels ? 1 : -1;
    }
    sceIoClose(fd);
}

void vlib_scan(void)
{
    vlib_thumbs(0);
    for (int i = 0; i < LOADED; i++) loaded[i].index = -1;
    video_count = 0;
    walk(video_dir, VCAT_OTHER, 0);
    for (int i = 0; i < video_count; i++) read_thumb_head(&videos[i]);
    load_resume();
}

// ---- Thumbnails -----------------------------------------------------------

typedef struct { u32 *px; int done; } Grab;

// Box-filters the decoded picture down to the thumbnail.
static void grab(const u32 *src, int w, int h, void *ctx)
{
    Grab *g = ctx;
    if (g->done || !src) return;
    for (int y = 0; y < THUMB_H; y++) {
        int y0 = y * h / THUMB_H, y1 = (y + 1) * h / THUMB_H;
        for (int x = 0; x < THUMB_W; x++) {
            int x0 = x * w / THUMB_W, x1 = (x + 1) * w / THUMB_W;
            u32 r = 0, gg = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    u32 c = src[yy * AVC_STRIDE + xx];
                    r += c & 0xFF; gg += (c >> 8) & 0xFF; b += (c >> 16) & 0xFF; n++;
                }
            if (!n) n = 1;
            g->px[y * THUMB_W + x] = (r / n) | (gg / n) << 8 | (b / n) << 16 | 0xFF000000;
        }
    }
    g->done = 1;
}

static void make_thumb(Video *v)
{
    static u32 px[THUMB_W * THUMB_H];
    ThumbHead h = { THUMB_MAGIC, v->modified, 0, 0, 0, 0 };
    Mp4 m;
    if (mp4_open(&m, v->path) == 0) {
        h.duration_ms = m.duration_ms;
        h.width = m.width;
        h.height = m.height;
        if (avc_open(&m) == 0) {
            SceUID fd = sceIoOpen(v->path, PSP_O_RDONLY, 0);
            int s = mp4_keyframe_before(&m, mp4_sample_at(&m.video, m.duration_ms / 10));
            u32 cap = 0;
            for (int i = s; i < m.video.count && i < s + 32; i++) if (m.video.size[i] > cap) cap = m.video.size[i];
            u8 *buf = memalign(64, cap + 64);
            Grab g = { px, 0 };
            // B-frames hold the first picture back for a few samples.
            for (int i = s; fd >= 0 && buf && !g.done && i < m.video.count && i < s + 32; i++) {
                if (sceIoLseek32(fd, m.video.offset[i], PSP_SEEK_SET) != (int)m.video.offset[i]) break;
                if (sceIoRead(fd, buf, m.video.size[i]) != (int)m.video.size[i]) break;
                if (avc_decode(buf, m.video.size[i], i == s, 1, grab, &g) < 0) break;
            }
            h.has_pixels = g.done;
            free(buf);
            if (fd >= 0) sceIoClose(fd);
            avc_close();
        }
        mp4_close(&m);
    }
    char path[300];
    thumb_path(v, path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, &h, sizeof(h));
        if (h.has_pixels) sceIoWrite(fd, px, sizeof(px));
        sceIoClose(fd);
    }
    v->duration_ms = h.duration_ms;
    v->width = h.width;
    v->height = h.height;
    v->thumb = h.has_pixels ? 1 : -1;
}

static int worker_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        if (!thumbs_on) { sceKernelDelayThread(50000); continue; }
        sceKernelWaitSema(busy, 1, NULL);
        int done = 1;
        for (int i = 0; i < video_count && thumbs_on; i++) {
            if (videos[i].thumb) continue;
            make_thumb(&videos[i]);
            done = 0;
            break;
        }
        sceKernelSignalSema(busy, 1);
        sceKernelDelayThread(done ? 200000 : 1000);
    }
    return 0;
}

void vlib_thumbs(int on)
{
    thumbs_on = on;
    if (!on && busy >= 0) {
        // Wait out the thumbnail in progress.
        sceKernelWaitSema(busy, 1, NULL);
        sceKernelSignalSema(busy, 1);
    }
}

const Texture *vlib_thumb(int index)
{
    if (index < 0 || index >= video_count || videos[index].thumb != 1) return NULL;
    use_clock++;
    int slot = 0;
    for (int i = 0; i < LOADED; i++) {
        if (loaded[i].index == index) { loaded[i].used = use_clock; return loaded[i].tex; }
        if (loaded[i].used < loaded[slot].used) slot = i;
    }
    Loaded *l = &loaded[slot];
    if (!l->tex) l->tex = tex_create(THUMB_W, THUMB_H);
    l->index = -1;
    char path[300];
    thumb_path(&videos[index], path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    ThumbHead h;
    int ok = sceIoRead(fd, &h, sizeof(h)) == sizeof(h) && h.has_pixels;
    for (int y = 0; ok && y < THUMB_H; y++)
        ok = sceIoRead(fd, l->tex->px + y * l->tex->tw, THUMB_W * 4) == THUMB_W * 4;
    sceIoClose(fd);
    if (!ok) { videos[index].thumb = 0; return NULL; }
    tex_upload(l->tex);
    l->index = index;
    l->used = use_clock;
    return l->tex;
}

void vlib_init(const char *data, const char *dir)
{
    snprintf(data_dir, sizeof(data_dir), "%s", data);
    snprintf(video_dir, sizeof(video_dir), "%s", dir);
    char path[300];
    snprintf(path, sizeof(path), "%s/thumbs", data_dir);
    sceIoMkdir(path, 0777);
    busy = sceKernelCreateSema("vthumbs", 0, 1, 1, NULL);
    vlib_scan();
    worker = sceKernelCreateThread("boro_thumbs", worker_thread, 0x20, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (worker >= 0) sceKernelStartThread(worker, 0, NULL);
}
