#include "vlib.h"

#include <malloc.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "avc.h"
#include "image.h"
#include "mp4.h"
#include "settings.h"

#define THUMB_W 160
#define THUMB_H 90
#define THUMB_MAGIC 0x32485442 // "BTH2": BTH1 files were made while decoding was broken
#define LOADED 20              // thumbnail textures kept in memory (Home shows about 14)

Video *videos;
int video_count;
Show *shows;
int show_count;
static int show_cap;

static char data_dir[256], video_dir[256], picture_dir[256];
static SceUID busy = -1;
static volatile int thumbs_on;
static SceUID worker = -1;

typedef struct { int index; Texture *tex; u32 used; } Loaded;
static Loaded loaded[LOADED];
static u32 use_clock;

// Seconds since about 1997: fits a u32 for a long while.
static u32 tick_seconds(u64 tick) { return (u32)(tick / 1000000 - 63000000000ULL); }
static u32 now_seconds(void) { u64 t; sceRtcGetCurrentTick(&t); return tick_seconds(t); }

static void count_watched(void);

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
        int fit = -1;
        sscanf(a, "%u\t%u\t%d", &pos, &when, &fit);
        for (int i = 0; i < video_count; i++)
            if (!strcmp(videos[i].path, line)) { videos[i].resume_ms = pos; videos[i].watched = when; videos[i].fit = fit; }
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
        if (videos[i].watched)
            fprintf(f, "%s\t%u\t%u\t%d\n", videos[i].path, (unsigned)videos[i].resume_ms, (unsigned)videos[i].watched, videos[i].fit);
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
    count_watched();
}

void vlib_set_fit(int index, int fit)
{
    if (index < 0 || index >= video_count) return;
    videos[index].fit = fit;
    save_resume();
}

int vlib_continue(void)
{
    int best = -1;
    if (!settings.resume_videos) return -1;
    for (int i = 0; i < video_count; i++)
        if (videos[i].resume_ms && (best < 0 || videos[i].watched > videos[best].watched)) best = i;
    return best;
}

// ---- Shows --------------------------------------------------------------

static void count_watched(void)
{
    for (int i = 0; i < show_count; i++) shows[i].watched = 0;
    for (int i = 0; i < video_count; i++)
        if (videos[i].show >= 0 && videos[i].watched && !videos[i].resume_ms) shows[videos[i].show].watched++;
}

static int cmp_episode(const void *a, const void *b)
{
    const Video *x = &videos[*(const int *)a], *y = &videos[*(const int *)b];
    if (x->season != y->season) return x->season - y->season;
    if (x->episode != y->episode) return x->episode - y->episode;
    return strcasecmp(x->title, y->title);
}

int vlib_episodes(int show, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < video_count && n < max; i++) if (videos[i].show == show) out[n++] = i;
    qsort(out, n, sizeof(int), cmp_episode);
    return n;
}

int vlib_next_up(int show)
{
    static int eps[512];
    int n = vlib_episodes(show, eps, 512);
    if (!n) return -1;
    int last = -1;
    for (int i = 0; i < n; i++) {
        const Video *v = &videos[eps[i]];
        if (!v->watched) continue;
        if (last < 0 || v->watched > videos[eps[last]].watched) last = i;
    }
    if (last < 0) return eps[0];
    if (videos[eps[last]].resume_ms && settings.resume_videos) return eps[last];
    return eps[last + 1 < n ? last + 1 : last];
}

u32 vlib_last_used(int show)
{
    u32 best = 0;
    for (int i = 0; i < video_count; i++) {
        if (videos[i].show != show) continue;
        u32 k = videos[i].watched > videos[i].modified ? videos[i].watched : videos[i].modified;
        if (k > best) best = k;
    }
    return best;
}

// ---- Posters --------------------------------------------------------------

#define POSTERS 16 // poster textures kept in memory

typedef struct { u32 key; Texture *tex; u32 used; int state; } Poster; // state 1 loaded, -1 none
static Poster posters[POSTERS];
static u32 poster_clock;
// Paths already found to have no poster, so they aren't looked for again.
static u32 missing[256];
static int missing_count;

static void posters_forget(void)
{
    for (int i = 0; i < POSTERS; i++) posters[i].key = 0;
    missing_count = 0;
}

// Center-crops and box-filters an RGBA picture into the poster texture.
static void fill_poster(Texture *t, const u8 *px, int w, int h)
{
    float want = (float)POSTER_W / POSTER_H;
    int cw = w, ch = h, cx = 0, cy = 0;
    if ((float)w / h > want) { cw = h * want; cx = (w - cw) / 2; }
    else { ch = w / want; cy = (h - ch) / 2; }
    for (int y = 0; y < POSTER_H; y++) {
        int y0 = cy + y * ch / POSTER_H, y1 = cy + (y + 1) * ch / POSTER_H;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < POSTER_W; x++) {
            int x0 = cx + x * cw / POSTER_W, x1 = cx + (x + 1) * cw / POSTER_W;
            if (x1 <= x0) x1 = x0 + 1;
            u32 r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const u8 *c = px + (yy * w + xx) * 4;
                    r += c[0]; g += c[1]; b += c[2]; n++;
                }
            t->px[y * t->tw + x] = (r / n) | (g / n) << 8 | (b / n) << 16 | 0xFF000000;
        }
    }
    tex_upload(t);
}

// stem: a path without its extension; tries .jpg, .jpeg and .png.
static const Texture *poster(const char *const stems[], int n)
{
    u32 key = fnv(stems[0]) | 1;
    poster_clock++;
    int slot = 0;
    for (int i = 0; i < POSTERS; i++) {
        if (posters[i].key == key) { posters[i].used = poster_clock; return posters[i].state > 0 ? posters[i].tex : NULL; }
        if (posters[i].used < posters[slot].used) slot = i;
    }
    for (int i = 0; i < missing_count; i++) if (missing[i] == key) return NULL;
    static const char *const ext[] = { ".jpg", ".jpeg", ".png", ".JPG", ".PNG" };
    char path[300];
    u8 *px = NULL;
    int w = 0, h = 0;
    for (int i = 0; i < n && !px; i++)
        for (int e = 0; e < 5 && !px; e++) {
            snprintf(path, sizeof(path), "%s%s", stems[i], ext[e]);
            SceIoStat st;
            if (sceIoGetstat(path, &st) < 0) continue;
            px = image_load(path, POSTER_W * 2, &w, &h);
        }
    if (!px || w <= 0 || h <= 0) {
        free(px);
        if (missing_count < 256) missing[missing_count++] = key;
        return NULL;
    }
    Poster *p = &posters[slot];
    if (!p->tex) p->tex = tex_create(POSTER_W, POSTER_H);
    fill_poster(p->tex, px, w, h);
    free(px);
    p->key = key;
    p->used = poster_clock;
    p->state = 1;
    return p->tex;
}

const Texture *vlib_show_poster(int show)
{
    if (show < 0 || show >= show_count) return NULL;
    char a[300], b[300], c[300];
    snprintf(a, sizeof(a), "%s/cover", shows[show].dir);
    snprintf(b, sizeof(b), "%s/poster", shows[show].dir);
    snprintf(c, sizeof(c), "%s/folder", shows[show].dir);
    // Or a picture named after the show beside its folder (Anime/<Show>.jpg).
    const char *const stems[] = { a, b, c, shows[show].dir };
    return poster(stems, 4);
}

const Texture *vlib_video_poster(int index)
{
    if (index < 0 || index >= video_count) return NULL;
    char a[300];
    snprintf(a, sizeof(a), "%s", videos[index].path);
    char *dot = strrchr(a, '.');
    if (dot) *dot = 0;
    const char *const stems[] = { a };
    return poster(stems, 1);
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

static int starts(const char *s, const char *word) { return !strncasecmp(s, word, strlen(word)); }

// From a folder's name, loosely: "Movies", "movie", "Films", "Music Videos",
// "TV Shows" and the like all count.
static int category_of(const char *name)
{
    while (*name == ' ' || *name == '_' || *name == '-') name++;
    if (starts(name, "movie") || starts(name, "film")) return VCAT_MOVIES;
    if (starts(name, "music")) return VCAT_MUSIC;
    if (starts(name, "show") || starts(name, "tv") || starts(name, "series")) return VCAT_SHOWS;
    if (starts(name, "clip")) return VCAT_CLIPS;
    return VCAT_OTHER;
}

static int cap;

// A number after a word like "Season" or "Ep", or 0.
static int number_after(const char *s, const char *const words[], int n)
{
    for (const char *p = s; *p; p++) {
        if (p != s && isalnum((u8)p[-1])) continue;
        for (int i = 0; i < n; i++) {
            int l = strlen(words[i]);
            if (strncasecmp(p, words[i], l)) continue;
            const char *q = p + l;
            while (*q == ' ' || *q == '.' || *q == '_' || *q == '-') q++;
            if (isdigit((u8)*q)) return atoi(q);
        }
    }
    return 0;
}

// "Season 2", "Series 2" or "S2" as a folder name.
static int season_of_folder(const char *name)
{
    static const char *const w[] = { "season", "series" };
    int n = number_after(name, w, 2);
    if (n) return n;
    if ((name[0] == 'S' || name[0] == 's') && isdigit((u8)name[1])) return atoi(name + 1);
    return 0;
}

// S01E03, 1x03, "Episode 3", "Ep 3" or "E03" in a file name.
static void episode_of_name(const char *name, int *season, int *episode)
{
    for (const char *p = name; *p; p++) {
        if (p != name && isalnum((u8)p[-1])) continue;
        if ((*p == 'S' || *p == 's') && isdigit((u8)p[1])) {
            char *e;
            long sn = strtol(p + 1, &e, 10);
            if ((*e == 'E' || *e == 'e') && isdigit((u8)e[1])) { *season = sn; *episode = atoi(e + 1); return; }
        }
        if (isdigit((u8)*p)) {
            char *e;
            long sn = strtol(p, &e, 10);
            if ((*e == 'x' || *e == 'X') && isdigit((u8)e[1]) && sn < 100) { *season = sn; *episode = atoi(e + 1); return; }
        }
    }
    static const char *const w[] = { "episode", "ep", "e" };
    int n = number_after(name, w, 3);
    if (n) *episode = n;
}

static int add_show(const char *name, const char *dir)
{
    if (show_count == show_cap) {
        show_cap = show_cap ? show_cap * 2 : 16;
        shows = realloc(shows, show_cap * sizeof(Show));
    }
    Show *s = &shows[show_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    for (char *p = s->name; *p; p++) if (*p == '_') *p = ' ';
    snprintf(s->dir, sizeof(s->dir), "%s", dir);
    return show_count++;
}

// shows_root: dir is the Shows folder itself, so each folder in it is a show.
// A 1-3 digit number standing on its own in a file name ("FLCL 01",
// "Show - 3"), not a year, "x264" or "480p".
static int short_number(const char *name)
{
    const char *end = strrchr(name, '.');
    if (!end) end = name + strlen(name);
    for (const char *p = name; p < end; p++) {
        if (!isdigit((u8)*p) || (p > name && isalnum((u8)p[-1]))) continue;
        const char *q = p;
        while (q < end && isdigit((u8)*q)) q++;
        if (q - p <= 3 && (q == end || !isalnum((u8)*q))) return 1;
        p = q - 1;
    }
    return 0;
}

// A folder of episodes outside a TV folder (say MOVIE/Anime/<Show>): it has
// season folders, or two or more numbered episodes, or one with a cover.
static int looks_like_show(const char *dir)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    SceIoDirent ent;
    int episodes = 0, seasons = 0, cover = 0;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (ent.d_name[0] == '.') continue;
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            if (season_of_folder(ent.d_name)) seasons++;
        } else if (is_mp4(ent.d_name)) {
            int sn = 0, ep = 0;
            episode_of_name(ent.d_name, &sn, &ep);
            if (ep > 0 || short_number(ent.d_name)) episodes++;
        } else if (!strncasecmp(ent.d_name, "cover.", 6) || !strncasecmp(ent.d_name, "poster.", 7) ||
                   !strncasecmp(ent.d_name, "folder.", 7)) {
            cover = 1;
        }
    }
    sceIoDclose(d);
    return seasons > 0 || episodes >= 2 || (cover && episodes > 0);
}

static void walk(const char *dir, int category, int show, int season, int shows_root, int depth)
{
    if (depth > 5) return;
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
            if (strlen(path) >= sizeof(shows->dir)) continue;
            // The first folder down with a known name decides, at any depth.
            if (category == VCAT_OTHER) {
                int c = category_of(ent.d_name);
                walk(path, c, -1, 0, c == VCAT_SHOWS, depth + 1);
            } else if (shows_root) {
                walk(path, category, add_show(ent.d_name, path), 0, 0, depth + 1);
            } else if (show < 0 && category != VCAT_CLIPS && looks_like_show(path)) {
                walk(path, VCAT_SHOWS, add_show(ent.d_name, path), 0, 0, depth + 1);
            } else {
                int sn = show >= 0 ? season_of_folder(ent.d_name) : 0;
                walk(path, category, show, sn ? sn : season, 0, depth + 1);
            }
        } else if (is_mp4(ent.d_name) && strlen(path) < sizeof(videos->path)) {
            if (video_count == cap) {
                cap = cap ? cap * 2 : 32;
                videos = realloc(videos, cap * sizeof(Video));
            }
            Video *v = &videos[video_count++];
            memset(v, 0, sizeof(*v));
            strcpy(v->path, path);
            v->fit = -1;
            make_title(v->title, sizeof(v->title), ent.d_name);
            v->category = category;
            v->show = show;
            if (show >= 0) {
                v->season = season;
                episode_of_name(ent.d_name, &v->season, &v->episode);
                shows[show].count++;
            }
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
        // A thumbnail that failed before gets another try on each rescan.
        v->thumb = h.has_pixels ? 1 : 0;
    }
    sceIoClose(fd);
}

void vlib_scan(void)
{
    vlib_thumbs(0);
    for (int i = 0; i < LOADED; i++) loaded[i].index = -1;
    video_count = 0;
    show_count = 0;
    posters_forget();
    walk(video_dir, VCAT_OTHER, -1, 0, 0, 0);
    // Videos among the photos (phone clips) are clips too.
    if (*picture_dir) walk(picture_dir, VCAT_CLIPS, -1, 0, 0, 0);
    for (int i = 0; i < video_count; i++) read_thumb_head(&videos[i]);
    load_resume();
    count_watched();
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

// A picture saved next to the video (Name.jpg, as the Video Wizard writes),
// center-cropped into the thumbnail. Returns 1 if there was one.
static int sidecar_thumb(const Video *v, u32 *px)
{
    static const char *const ext[] = { ".jpg", ".jpeg", ".png", ".JPG", ".PNG" };
    char path[300];
    for (int e = 0; e < 5; e++) {
        snprintf(path, sizeof(path), "%s", v->path);
        char *dot = strrchr(path, '.');
        if (!dot || (int)(dot - path) + 6 >= (int)sizeof(path)) return 0;
        strcpy(dot, ext[e]);
        SceIoStat st;
        if (sceIoGetstat(path, &st) < 0) continue;
        int w = 0, h = 0;
        u8 *img = image_load(path, THUMB_H * 2, &w, &h);
        if (!img) continue;
        float want = (float)THUMB_W / THUMB_H;
        int cw = w, ch = h, cx = 0, cy = 0;
        if ((float)w / h > want) { cw = h * want; cx = (w - cw) / 2; }
        else { ch = w / want; cy = (h - ch) / 2; }
        for (int y = 0; y < THUMB_H; y++) {
            int y0 = cy + y * ch / THUMB_H, y1 = cy + (y + 1) * ch / THUMB_H;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < THUMB_W; x++) {
                int x0 = cx + x * cw / THUMB_W, x1 = cx + (x + 1) * cw / THUMB_W;
                if (x1 <= x0) x1 = x0 + 1;
                u32 r = 0, g = 0, b = 0, n = 0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        const u8 *c = img + (yy * w + xx) * 4;
                        r += c[0]; g += c[1]; b += c[2]; n++;
                    }
                px[y * THUMB_W + x] = (r / n) | (g / n) << 8 | (b / n) << 16 | 0xFF000000;
            }
        }
        free(img);
        return 1;
    }
    return 0;
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
        if (sidecar_thumb(v, px)) h.has_pixels = 1;
        else if (avc_open(&m) == 0) {
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

const char *vlib_dir(void) { return video_dir; }

void vlib_init(const char *data, const char *dir, const char *pictures)
{
    snprintf(data_dir, sizeof(data_dir), "%s", data);
    snprintf(video_dir, sizeof(video_dir), "%s", dir);
    snprintf(picture_dir, sizeof(picture_dir), "%s", pictures);
    char path[300];
    snprintf(path, sizeof(path), "%s/thumbs", data_dir);
    sceIoMkdir(path, 0777);
    busy = sceKernelCreateSema("vthumbs", 0, 1, 1, NULL);
    vlib_scan();
    worker = sceKernelCreateThread("boro_thumbs", worker_thread, 0x20, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (worker >= 0) sceKernelStartThread(worker, 0, NULL);
}
