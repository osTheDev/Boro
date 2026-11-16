#include "glib.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include "image.h"

Game *games;
int game_count;

static char log_path[256], now_path[256], app_dir[256], dev[8] = "ms0";

// ---- Playtime log -------------------------------------------------------------

// One line per session, from the plugin:
// started \t seconds \t disc_id \t category \t title \t path
typedef struct {
    char path[256], id[16], title[96], last[20];
    u32 seconds, sessions;
} Played;

static Played *played;
static int played_count, played_cap;

static Played *played_for(const char *path, const char *id, const char *title)
{
    for (int i = 0; i < played_count; i++)
        if (!strcasecmp(played[i].path, path)) return &played[i];
    if (played_count == played_cap) {
        int cap = played_cap ? played_cap * 2 : 32;
        Played *p = realloc(played, cap * sizeof(Played));
        if (!p) return NULL;
        played = p;
        played_cap = cap;
    }
    Played *p = &played[played_count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->path, sizeof(p->path), "%s", path);
    snprintf(p->id, sizeof(p->id), "%s", id);
    snprintf(p->title, sizeof(p->title), "%s", title);
    return p;
}

static char *read_text(const char *path, int *len)
{
    *len = 0;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *buf = size >= 0 ? malloc(size + 1) : NULL;
    if (buf) {
        *len = sceIoRead(fd, buf, size);
        if (*len < 0) *len = 0;
        buf[*len] = 0;
    }
    sceIoClose(fd);
    return buf;
}

// The plugin leaves the latest session in playtime_now.txt; no game runs
// while Boro does, so it's safe to move it into the log here.
static void fold_now(void)
{
    int len;
    char *now = read_text(now_path, &len);
    if (!now) return;
    if (len > 0) {
        SceIoStat st;
        int fresh = sceIoGetstat(log_path, &st) < 0;
        SceUID fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) {
            if (fresh) {
                const char *head = "# started\tseconds\tdisc_id\tcategory\ttitle\tpath\n";
                sceIoWrite(fd, head, strlen(head));
            }
            sceIoWrite(fd, now, len);
            sceIoClose(fd);
        }
    }
    free(now);
    sceIoRemove(now_path);
}

static void read_log(void)
{
    played_count = 0;
    int len;
    char *buf = read_text(log_path, &len);
    if (!buf) return;
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *f[6] = { 0 };
        int n = 0;
        for (char *p = line; n < 6 && p;) {
            f[n++] = p;
            p = strchr(p, '\t');
            if (p) *p++ = 0;
        }
        if (n == 6 && line[0] != '#') {
            char *cr = strchr(f[5], '\r');
            if (cr) *cr = 0;
            Played *p = played_for(f[5], f[2], f[4]);
            if (p) {
                p->seconds += strtoul(f[1], NULL, 10);
                p->sessions++;
                if (strcmp(f[0], p->last) > 0) snprintf(p->last, sizeof(p->last), "%s", f[0]);
            }
        }
        line = next;
    }
    free(buf);
}

// Totals onto the games: by path, or for PSP games by disc ID (a game that
// was moved or turned from ISO into CSO keeps its time). Homebrew often
// shares a made-up disc ID, so it only matches by path.
static void apply_log(void)
{
    for (int i = 0; i < game_count; i++) {
        Game *g = &games[i];
        g->seconds = g->sessions = 0;
        g->last[0] = 0;
        for (int j = 0; j < played_count; j++) {
            Played *p = &played[j];
            int same = !strcasecmp(p->path, g->path) ||
                       (g->type == GTYPE_PSP && g->disc_id[0] && !strcmp(p->id, g->disc_id));
            if (!same) continue;
            g->seconds += p->seconds;
            g->sessions += p->sessions;
            if (strcmp(p->last, g->last) > 0) snprintf(g->last, sizeof(g->last), "%s", p->last);
        }
    }
}

const char *glib_last_title(void)
{
    Played *best = NULL;
    for (int i = 0; i < played_count; i++)
        if (!best || strcmp(played[i].last, best->last) > 0) best = &played[i];
    return best ? best->title : NULL;
}

int glib_last_game(void)
{
    int best = -1;
    for (int i = 0; i < game_count; i++)
        if (games[i].last[0] && (best < 0 || strcmp(games[i].last, games[best].last) > 0)) best = i;
    return best;
}

// ---- Reading games ------------------------------------------------------------

// Little-endian u32 from any address: ISO directory records put them at odd
// offsets, and the PSP faults on unaligned word loads.
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

// PARAM.SFO: copies a string value (TITLE, DISC_ID, CATEGORY).
static void sfo_get(const u8 *sfo, int size, const char *key, char *out, int out_size)
{
    if (size < 20 || memcmp(sfo, "\0PSF", 4)) return;
    u32 keys = rd32(sfo + 8), data = rd32(sfo + 12), n = rd32(sfo + 16);
    for (u32 i = 0; i < n && 20 + i * 16 + 16 <= (u32)size; i++) {
        const u8 *e = sfo + 20 + i * 16;
        u32 k = keys + (e[0] | e[1] << 8), len = rd32(e + 4), off = data + rd32(e + 12);
        if (k >= (u32)size || off + len > (u32)size || strcmp((const char *)sfo + k, key)) continue;
        if ((int)len >= out_size) len = out_size - 1;
        memcpy(out, sfo + off, len);
        out[len] = 0;
        return;
    }
}

// An ISO, or a CSO read a 2048-byte block at a time.
typedef struct {
    SceUID fd;
    int cso, align;
    u32 blocks;
    u8 zbuf[2048 + 1024];
} Disc;

static int disc_open(Disc *d, const char *path)
{
    d->fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (d->fd < 0) return 0;
    u8 head[24];
    d->cso = 0;
    if (sceIoRead(d->fd, head, 24) == 24 && !memcmp(head, "CISO", 4)) {
        u32 block_size = rd32(head + 16);
        u64 total = rd32(head + 8) | (u64)rd32(head + 12) << 32;
        if (block_size != 2048) { sceIoClose(d->fd); return 0; }
        d->cso = 1;
        d->align = head[21];
        d->blocks = (u32)(total >> 11);
    }
    return 1;
}

static void disc_close(Disc *d) { sceIoClose(d->fd); }

static int disc_sector(Disc *d, u32 lba, u8 *out)
{
    if (!d->cso) {
        sceIoLseek(d->fd, (s64)lba * 2048, PSP_SEEK_SET);
        return sceIoRead(d->fd, out, 2048) == 2048;
    }
    if (lba >= d->blocks) return 0;
    u32 idx[2];
    sceIoLseek(d->fd, 24 + (s64)lba * 4, PSP_SEEK_SET);
    if (sceIoRead(d->fd, idx, 8) != 8) return 0;
    int plain = idx[0] >> 31;
    u64 pos = (u64)(idx[0] & 0x7FFFFFFF) << d->align;
    u32 len = (u32)(((u64)(idx[1] & 0x7FFFFFFF) << d->align) - pos);
    if (plain || len >= 2048) {
        sceIoLseek(d->fd, pos, PSP_SEEK_SET);
        return sceIoRead(d->fd, out, 2048) == 2048;
    }
    if (len > sizeof(d->zbuf)) len = sizeof(d->zbuf);
    sceIoLseek(d->fd, pos, PSP_SEEK_SET);
    int got = sceIoRead(d->fd, d->zbuf, len);
    if (got <= 0) return 0;
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK) return 0;
    z.next_in = d->zbuf;
    z.avail_in = got;
    z.next_out = out;
    z.avail_out = 2048;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return (r == Z_STREAM_END || r == Z_OK || r == Z_BUF_ERROR) && z.avail_out == 0;
}

// A file inside an ISO 9660 directory: where it starts and how big it is.
static int disc_find(Disc *d, u32 dir_lba, u32 dir_size, const char *name, u32 *lba, u32 *size)
{
    static u8 sec[2048];
    int nlen = strlen(name);
    for (u32 s = 0; s * 2048 < dir_size && s < 64; s++) {
        if (!disc_sector(d, dir_lba + s, sec)) return 0;
        for (int o = 0; o < 2048 && sec[o];) {
            int rec = sec[o], flen = sec[o + 32];
            const char *fn = (const char *)sec + o + 33;
            if (o + rec > 2048) break;
            if ((flen == nlen || (flen > nlen && fn[nlen] == ';')) && !strncasecmp(fn, name, nlen)) {
                *lba = rd32(sec + o + 2);
                *size = rd32(sec + o + 10);
                return 1;
            }
            o += rec;
        }
    }
    return 0;
}

static u8 *disc_file(Disc *d, u32 lba, u32 size, u32 max)
{
    if (!size || size > max) return NULL;
    u8 *buf = malloc((size + 2047) & ~2047);
    if (!buf) return NULL;
    for (u32 s = 0; s * 2048 < size; s++)
        if (!disc_sector(d, lba + s, buf + s * 2048)) { free(buf); return NULL; }
    return buf;
}

// Fits an icon into GICON_W x GICON_H, letterboxed (PS1 icons are square).
static u8 *fit_icon(const u8 *src, int w, int h)
{
    u8 *out = malloc(GICON_W * GICON_H * 4);
    if (!out) return NULL;
    for (int i = 0; i < GICON_W * GICON_H; i++) ((u32 *)out)[i] = 0xFF241C1A;
    float k = (float)GICON_W / w < (float)GICON_H / h ? (float)GICON_W / w : (float)GICON_H / h;
    int ow = w * k + 0.5f, oh = h * k + 0.5f, ox = (GICON_W - ow) / 2, oy = (GICON_H - oh) / 2;
    for (int y = 0; y < oh; y++) {
        float sy = (y + 0.5f) / k - 0.5f;
        int y0 = sy < 0 ? 0 : (int)sy, y1 = y0 + 1 < h ? y0 + 1 : y0;
        float fy = sy - y0 < 0 ? 0 : sy - y0;
        for (int x = 0; x < ow; x++) {
            float sx = (x + 0.5f) / k - 0.5f;
            int x0 = sx < 0 ? 0 : (int)sx, x1 = x0 + 1 < w ? x0 + 1 : x0;
            float fx = sx - x0 < 0 ? 0 : sx - x0;
            const u8 *a = src + (y0 * w + x0) * 4, *b = src + (y0 * w + x1) * 4;
            const u8 *c = src + (y1 * w + x0) * 4, *e = src + (y1 * w + x1) * 4;
            u8 *o = out + ((oy + y) * GICON_W + ox + x) * 4;
            float alpha = 0;
            for (int ch = 0; ch < 4; ch++) {
                float v = (a[ch] * (1 - fx) + b[ch] * fx) * (1 - fy) + (c[ch] * (1 - fx) + e[ch] * fx) * fy;
                if (ch == 3) alpha = v / 255;
                else o[ch] = (u8)v;
            }
            // Blend see-through icons over the dark backing.
            static const u8 back[3] = { 0x1A, 0x1C, 0x24 };
            for (int ch = 0; ch < 3; ch++) o[ch] = (u8)(o[ch] * alpha + back[ch] * (1 - alpha));
            o[3] = 255;
        }
    }
    return out;
}

static u8 *decode_icon(u8 *png, u32 size)
{
    if (!png) return NULL;
    int w = 0, h = 0;
    u8 *px = image_decode(png, size, 0, &w, &h);
    free(png);
    if (!px) return NULL;
    u8 *out = w > 0 && h > 0 ? fit_icon(px, w, h) : NULL;
    free(px);
    return out;
}

// Title, disc ID and type; with want_icon, the icon too.
static int read_iso(Game *g, int want_icon, u8 **icon)
{
    static Disc d;
    static u8 pvd[2048];
    if (!disc_open(&d, g->path)) return 0;
    u32 lba, size, dl, ds;
    int ok = 0;
    if (disc_sector(&d, 16, pvd) && !memcmp(pvd + 1, "CD001", 5) &&
        disc_find(&d, rd32(pvd + 156 + 2), rd32(pvd + 156 + 10), "PSP_GAME", &dl, &ds)) {
        if (!want_icon) {
            if (disc_find(&d, dl, ds, "PARAM.SFO", &lba, &size)) {
                u8 *sfo = disc_file(&d, lba, size, 64 * 1024);
                if (sfo) {
                    sfo_get(sfo, size, "TITLE", g->title, sizeof(g->title));
                    sfo_get(sfo, size, "DISC_ID", g->disc_id, sizeof(g->disc_id));
                    free(sfo);
                    ok = 1;
                }
            }
        } else if (disc_find(&d, dl, ds, "ICON0.PNG", &lba, &size)) {
            *icon = decode_icon(disc_file(&d, lba, size, 512 * 1024), size);
            ok = 1;
        }
    }
    disc_close(&d);
    return ok;
}

static int read_pbp(Game *g, int want_icon, u8 **icon)
{
    SceUID fd = sceIoOpen(g->path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    u32 head[10];
    int ok = 0;
    if (sceIoRead(fd, head, sizeof(head)) == sizeof(head) && head[0] == 0x50425000) {
        int part = want_icon ? 1 : 0; // ICON0 follows PARAM.SFO
        u32 off = head[2 + part], size = head[3 + part] - off;
        u8 *buf = size && size <= 512 * 1024 ? malloc(size) : NULL;
        if (buf) {
            sceIoLseek32(fd, off, PSP_SEEK_SET);
            if (sceIoRead(fd, buf, size) == (int)size) {
                ok = 1;
                if (want_icon) {
                    *icon = decode_icon(buf, size);
                    buf = NULL;
                } else {
                    char cat[8] = "";
                    sfo_get(buf, size, "TITLE", g->title, sizeof(g->title));
                    sfo_get(buf, size, "DISC_ID", g->disc_id, sizeof(g->disc_id));
                    sfo_get(buf, size, "CATEGORY", cat, sizeof(cat));
                    g->type = !strcmp(cat, "ME") ? GTYPE_PS1 : !strcmp(cat, "MG") ? GTYPE_HOMEBREW : GTYPE_PSP;
                }
            }
            free(buf);
        } else if (want_icon) {
            ok = 1; // no icon in this one
        }
    }
    sceIoClose(fd);
    return ok;
}

// ---- Scanning ---------------------------------------------------------------------

static Game *found;
static int found_count, found_cap;
static volatile int scan_state; // 0 not started, 1 running, 2 list ready, 3 taken in

static Game *add_game(const char *path)
{
    if (found_count == found_cap) {
        int cap = found_cap ? found_cap * 2 : 64;
        Game *g = realloc(found, cap * sizeof(Game));
        if (!g) return NULL;
        found = g;
        found_cap = cap;
    }
    Game *g = &found[found_count++];
    memset(g, 0, sizeof(*g));
    snprintf(g->path, sizeof(g->path), "%s", path);
    return g;
}

static int ends_with(const char *s, const char *end)
{
    int a = strlen(s), b = strlen(end);
    return a >= b && !strcasecmp(s + a - b, end);
}

// The file name without its folder and extension, until the game says better.
static void title_from_name(Game *g, const char *name)
{
    snprintf(g->title, sizeof(g->title), "%s", name);
    char *dot = strrchr(g->title, '.');
    if (dot) *dot = 0;
}

static void scan_isos(const char *dir, int depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent ent;
    char path[256];
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (ent.d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            if (depth < 2) scan_isos(path, depth + 1);
        } else if (ends_with(ent.d_name, ".iso") || ends_with(ent.d_name, ".cso")) {
            Game *g = add_game(path);
            if (g) title_from_name(g, ent.d_name);
        }
    }
    sceIoDclose(d);
}

static void scan_eboots(const char *dir)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent ent;
    char path[256];
    SceIoStat st;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        const char *n = ent.d_name;
        // Not games: dot folders, 1.50 kxploit halves, ARK's own tools.
        if (n[0] == '.' || n[0] == '%' || n[0] == '_' || !strncasecmp(n, "ARK", 3) || !FIO_S_ISDIR(ent.d_stat.st_mode))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, n);
        if (!strcasecmp(path, app_dir)) continue; // Boro itself
        snprintf(path, sizeof(path), "%s/%s/EBOOT.PBP", dir, n);
        if (sceIoGetstat(path, &st) < 0) continue;
        Game *g = add_game(path);
        if (g) {
            g->type = GTYPE_HOMEBREW;
            snprintf(g->title, sizeof(g->title), "%s", n);
        }
    }
    sceIoDclose(d);
}

static int cmp_title(const void *a, const void *b)
{
    return strcasecmp(((const Game *)a)->title, ((const Game *)b)->title);
}

static int scan_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    char dir[64];
    snprintf(dir, sizeof(dir), "%s:/ISO", dev);
    scan_isos(dir, 0);
    snprintf(dir, sizeof(dir), "%s:/PSP/GAME", dev);
    scan_eboots(dir);
    for (int i = 0; i < found_count; i++) {
        Game *g = &found[i];
        if (ends_with(g->path, ".pbp")) read_pbp(g, 0, NULL);
        else read_iso(g, 0, NULL);
    }
    qsort(found, found_count, sizeof(Game), cmp_title);
    scan_state = 2;
    // Icons next, played games first so Recent fills in quickest. The list
    // itself is never moved again, so filling in entries is safe.
    while (scan_state != 3) sceKernelDelayThread(20000);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < found_count; i++) {
            Game *g = &found[i];
            if (g->icon || (pass == 0) != (g->last[0] != 0)) continue;
            u8 *px = NULL;
            if (ends_with(g->path, ".pbp")) read_pbp(g, 1, &px);
            else read_iso(g, 1, &px);
            g->icon_px = px;
            g->icon = px ? 1 : -1;
        }
    }
    return sceKernelExitDeleteThread(0);
}

void glib_init(const char *data_dir, const char *dir)
{
    snprintf(log_path, sizeof(log_path), "%s/playtime.txt", data_dir);
    snprintf(now_path, sizeof(now_path), "%s/playtime_now.txt", data_dir);
    snprintf(app_dir, sizeof(app_dir), "%s", dir);
    const char *colon = strchr(dir, ':');
    if (colon && colon - dir < (int)sizeof(dev)) snprintf(dev, sizeof(dev), "%.*s", (int)(colon - dir), dir);
    fold_now();
    read_log();
}

void glib_scan(void)
{
    if (scan_state) return;
    scan_state = 1;
    SceUID th = sceKernelCreateThread("boro_games", scan_thread, 0x30, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    else scan_state = 3;
}

int glib_ready(void) { return scan_state == 3; }

void glib_frame(void)
{
    if (scan_state == 2) {
        games = found;
        game_count = found_count;
        apply_log();
        scan_state = 3;
    }
    if (scan_state != 3) return;
    // A few icons a frame at most, so scrolling never stutters.
    int n = 0;
    for (int i = 0; i < game_count && n < 4; i++) {
        Game *g = &games[i];
        if (g->icon != 1) continue;
        g->tex = tex_create(GICON_W, GICON_H);
        if (g->tex) {
            for (int y = 0; y < GICON_H; y++)
                memcpy(g->tex->px + y * g->tex->tw, g->icon_px + y * GICON_W * 4, GICON_W * 4);
            tex_upload(g->tex);
        }
        free(g->icon_px);
        g->icon_px = NULL;
        g->icon = g->tex ? 2 : -1;
        n++;
    }
}

const Texture *glib_icon(int index)
{
    return index >= 0 && index < game_count && games[index].icon == 2 ? games[index].tex : NULL;
}

// ---- Formatting -----------------------------------------------------------------

void glib_format_time(char *out, int size, u32 seconds)
{
    u32 m = seconds / 60, h = m / 60;
    if (h) snprintf(out, size, "%luh %02lum", (unsigned long)h, (unsigned long)(m % 60));
    else if (m) snprintf(out, size, "%lum", (unsigned long)m);
    else snprintf(out, size, seconds ? "Under 1m" : "Not played");
}

// Days since 1970 for a calendar date.
static int day_number(int y, int m, int d)
{
    y -= m <= 2;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

void glib_format_last(char *out, int size, const char *last)
{
    int y, m, d;
    if (!last || sscanf(last, "%d-%d-%d", &y, &m, &d) != 3) { snprintf(out, size, "Never"); return; }
    ScePspDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    int ago = day_number(now.year, now.month, now.day) - day_number(y, m, d);
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (ago <= 0) snprintf(out, size, "Today");
    else if (ago == 1) snprintf(out, size, "Yesterday");
    else if (ago < 7) snprintf(out, size, "%d days ago", ago);
    else snprintf(out, size, "%s %d", months[(m - 1) % 12], d);
}
