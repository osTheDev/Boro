#include "photos.h"

#include <pspctrl.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "font.h"
#include "gfx.h"
#include "image.h"
#include <pspiofilemgr.h>
#include "plib.h"
#include "pviewer.h"
#include "ui.h"
#include "vlib.h"

#define BG HEX(0x0E0F13, 255)
#define ACCENT HEX(0x5B8CFF, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define DIM HEX(0x8A8D99, 255)
#define CHIP HEX(0x3A3D48, 255)

// Library: everything by the day it was taken, newest first, in a tight grid.
#define LIB_COLS 7
#define LIB_TILE 60
#define LIB_STEP 63
#define LIB_X 19
#define HEAD_H 20
#define CONTENT_Y 56
#define CONTENT_B (SCREEN_H - 20)

// Albums: cards three across; an open album is a grid of its photos.
#define CARD_W 144
#define CARD_H 66
#define CARD_STEP_X 152
#define CARD_STEP_Y 94
#define COLS 4
#define CELL_W (PTHUMB_W + 8)
#define CELL_H (PTHUMB_H + 24)

enum { VIEW_LIBRARY, VIEW_ALBUMS };

typedef struct { int photo, video; u32 day, time; } Item; // photo or video is -1
typedef struct { int head; int first, n; u32 day; float y; } Line;

static Font *f_head, *f_small, *f_label, *f_tiny;
static Texture *t_fill;
static int view, album = -1; // album: the open one, or -1
static int sel, album_sel;   // sel: in the Library or the open album; album_sel: on the Albums page
static float scroll;
static int viewing, view_from_library;
static int play = -1;
static int trash_open;     // Recently Deleted is open
static int confirm;        // 1: move the photo to Recently Deleted, 2: delete for good
static float hold_t;       // how long square has been held
static int hold_fired;

#define TRASH_KEEP 4

typedef struct { char file[300], orig[256], name[64]; } Trashed;
static Trashed trash[TRASH_KEEP + 1];
static int trash_count;
static Texture *trash_tex[TRASH_KEEP];
static int trash_tex_state[TRASH_KEEP]; // 0 not loaded, 1 loaded, -1 broken

static Item *items;
static int item_count;
static Line *lines;
static int line_count;
static int *item_line;
static int *run; // photo indexes in Library order, for the viewer
static int run_count;
static u32 today;

// ---- Dates ----------------------------------------------------------------

static u32 ymd_of(const ScePspDateTime *t) { return t->year * 10000 + t->month * 100 + t->day; }

// File times are kept as seconds since the same point as vlib and plib.
static u32 day_of_seconds(u32 s)
{
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    u64 tick = ((u64)s + 63000000000ULL) * 1000000ULL;
    sceRtcSetTick(&t, &tick);
    return ymd_of(&t);
}

static void day_label(u32 day, char *out, int size)
{
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *const wdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int y = day / 10000, m = day / 100 % 100, d = day % 100;
    if (day == today) { snprintf(out, size, "Today"); return; }
    // Yesterday: the day before, across a month's end too.
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    t.year = today / 10000; t.month = today / 100 % 100; t.day = today % 100;
    u64 tick;
    sceRtcGetTick(&t, &tick);
    sceRtcTickAddDays(&tick, &tick, -1);
    sceRtcSetTick(&t, &tick);
    if (day == ymd_of(&t)) { snprintf(out, size, "Yesterday"); return; }
    if (m < 1 || m > 12) { snprintf(out, size, "Undated"); return; }
    int wd = sceRtcGetDayOfWeek(y, m, d);
    if ((u32)y == today / 10000) snprintf(out, size, "%s, %s %d", wdays[wd % 7], months[m - 1], d);
    else snprintf(out, size, "%s %d, %d", months[m - 1], d, y);
}

// ---- Library ----------------------------------------------------------------

static int cmp_item(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    if (x->day != y->day) return x->day < y->day ? 1 : -1;
    if (x->time != y->time) return x->time < y->time ? 1 : -1;
    return 0;
}

static void build_library(void)
{
    free(items); free(lines); free(item_line); free(run);
    int clips = 0;
    for (int i = 0; i < video_count; i++) if (videos[i].category == VCAT_CLIPS) clips++;
    int cap = photo_count + clips;
    items = malloc((cap ? cap : 1) * sizeof(Item));
    item_line = malloc((cap ? cap : 1) * sizeof(int));
    run = malloc((photo_count ? photo_count : 1) * sizeof(int));
    item_count = 0;
    for (int i = 0; i < photo_count; i++) {
        const Photo *p = &photos[i];
        items[item_count++] = (Item){ i, -1, p->taken ? p->taken : day_of_seconds(p->modified), p->modified };
    }
    for (int i = 0; i < video_count; i++)
        if (videos[i].category == VCAT_CLIPS)
            items[item_count++] = (Item){ -1, i, day_of_seconds(videos[i].modified), videos[i].modified };
    qsort(items, item_count, sizeof(Item), cmp_item);

    // A heading for each day, then its rows.
    lines = malloc((item_count * 2 + 1) * sizeof(Line));
    line_count = 0;
    run_count = 0;
    float y = 0;
    for (int i = 0; i < item_count; i++) {
        if (!i || items[i].day != items[i - 1].day) {
            lines[line_count++] = (Line){ 1, i, 0, items[i].day, y };
            y += HEAD_H;
        }
        Line *l = &lines[line_count - 1];
        if (l->head || l->n == LIB_COLS || l->day != items[i].day) {
            lines[line_count++] = (Line){ 0, i, 0, items[i].day, y };
            y += LIB_STEP;
            l = &lines[line_count - 1];
        }
        l->n++;
        item_line[i] = line_count - 1;
        if (items[i].photo >= 0) run[run_count++] = items[i].photo;
    }
}

static int col_of(int i) { return i - lines[item_line[i]].first; }

// The item in the row step rows away (skipping headings), same column.
static int row_step(int i, int step)
{
    int l = item_line[i];
    do l += step; while (l >= 0 && l < line_count && lines[l].head);
    if (l < 0 || l >= line_count) return i;
    int c = col_of(i);
    return lines[l].first + (c < lines[l].n ? c : lines[l].n - 1);
}

// The first item of the next older month, or the top after the oldest.
static int next_month(int i)
{
    u32 month = items[i].day / 100;
    for (int k = i + 1; k < item_count; k++) if (items[k].day / 100 != month) return k;
    return 0;
}

static int library_update(const Input *in)
{
    u32 p = in->pressed, nav = in->nav;
    if (!item_count) return 0;
    if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
    if (nav & PSP_CTRL_RIGHT && sel < item_count - 1) sel++;
    if (nav & PSP_CTRL_UP) sel = row_step(sel, -1);
    if (nav & PSP_CTRL_DOWN) sel = row_step(sel, 1);
    if (p & PSP_CTRL_TRIANGLE) sel = next_month(sel);
    if (p & PSP_CTRL_CROSS) {
        const Item *it = &items[sel];
        if (it->video >= 0) {
            play = it->video;
        } else {
            int at = 0;
            for (int k = 0; k < run_count; k++) if (run[k] == it->photo) at = k;
            plib_thumbs(0); // the viewer gets the CPU to itself
            pviewer_open_list(run, run_count, at, "Library");
            viewing = 1;
            view_from_library = 1;
        }
    }
    return 0;
}

// ---- Albums -----------------------------------------------------------------

static int albums_update(const Input *in)
{
    u32 p = in->pressed, nav = in->nav;
    if (album >= 0) {
        int n = palbums[album].count;
        if (p & PSP_CTRL_CIRCLE) { album = -1; return 0; }
        if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
        if (nav & PSP_CTRL_RIGHT && sel < n - 1) sel++;
        if (nav & PSP_CTRL_UP && sel >= COLS) sel -= COLS;
        if (nav & PSP_CTRL_DOWN && sel / COLS < (n - 1) / COLS) sel = sel + COLS < n ? sel + COLS : n - 1;
        if (p & PSP_CTRL_CROSS && n) {
            plib_thumbs(0);
            pviewer_open(palbums[album].first, n, sel, palbums[album].name);
            viewing = 1;
            view_from_library = 0;
        }
        return 0;
    }
    int n = palbum_count + (trash_count ? 1 : 0);
    if (!n) return 0;
    if (album_sel >= n) album_sel = n - 1;
    if (nav & PSP_CTRL_LEFT && album_sel > 0) album_sel--;
    if (nav & PSP_CTRL_RIGHT && album_sel < n - 1) album_sel++;
    if (nav & PSP_CTRL_UP && album_sel >= 3) album_sel -= 3;
    if (nav & PSP_CTRL_DOWN && album_sel / 3 < (n - 1) / 3) album_sel = album_sel + 3 < n ? album_sel + 3 : n - 1;
    if (p & PSP_CTRL_CROSS && album_sel == palbum_count) { trash_open = 1; sel = 0; }
    else if (p & PSP_CTRL_CROSS) { album = album_sel; sel = 0; scroll = 0; }
    return 0;
}

// ---- Recently Deleted -------------------------------------------------------------
//
// Deleted photos move to Boro's data/trash folder, out of PICTURE;
// list.txt there holds "file<TAB>original path", oldest first. Only the last
// TRASH_KEEP stay: a newer one deletes the oldest for good.


// Renames, or where the memory stick won't rename across folders, copies
// and then removes the original once the copy is whole.
static int move_file(const char *from, const char *to)
{
    if (sceIoRename(from, to) >= 0) return 0;
    SceUID in = sceIoOpen(from, PSP_O_RDONLY, 0);
    if (in < 0) return -1;
    SceUID out = sceIoOpen(to, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (out < 0) { sceIoClose(in); return -1; }
    static u8 buf[128 * 1024];
    int n, ok = 1;
    while ((n = sceIoRead(in, buf, sizeof(buf))) > 0)
        if (sceIoWrite(out, buf, n) != n) { ok = 0; break; }
    if (n < 0) ok = 0;
    sceIoClose(in);
    sceIoClose(out);
    if (!ok) { sceIoRemove(to); return -1; }
    sceIoRemove(from);
    return 0;
}

static void trash_dir(char *out, int size) { snprintf(out, size, "%s", plib_trash_dir()); }

static void trash_save(void)
{
    char path[320];
    trash_dir(path, sizeof(path));
    strcat(path, "/list.txt");
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < trash_count; i++) fprintf(f, "%s\t%s\n", trash[i].file, trash[i].orig);
    fclose(f);
}

static void trash_forget_textures(void) { for (int i = 0; i < TRASH_KEEP; i++) trash_tex_state[i] = 0; }

static void trash_load(void)
{
    char path[320], line[600];
    trash_dir(path, sizeof(path));
    strcat(path, "/list.txt");
    trash_count = 0;
    trash_forget_textures();
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f) && trash_count < TRASH_KEEP) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        SceIoStat st;
        if (sceIoGetstat(line, &st) < 0) continue; // removed by hand
        Trashed *t = &trash[trash_count++];
        snprintf(t->file, sizeof(t->file), "%.299s", line);
        snprintf(t->orig, sizeof(t->orig), "%s", tab + 1);
        const char *slash = strrchr(t->orig, '/');
        snprintf(t->name, sizeof(t->name), "%.63s", slash ? slash + 1 : t->orig);
    }
    fclose(f);
}

static void trash_purge(int i)
{
    sceIoRemove(trash[i].file);
    memmove(&trash[i], &trash[i + 1], (trash_count - i - 1) * sizeof(Trashed));
    trash_count--;
    trash_forget_textures();
    trash_save();
}

static int trash_put(int photo)
{
    char dir[320];
    trash_dir(dir, sizeof(dir));
    ScePspDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    Trashed t;
    snprintf(t.orig, sizeof(t.orig), "%s", photos[photo].path);
    snprintf(t.name, sizeof(t.name), "%s", photos[photo].name);
    snprintf(t.file, sizeof(t.file), "%.200s/%02d%02d%02d%02d_%.63s", dir, now.day, now.hour, now.minute, now.second, photos[photo].name);
    if (move_file(t.orig, t.file) < 0) return -1;
    trash[trash_count++] = t;
    while (trash_count > TRASH_KEEP) trash_purge(0);
    trash_forget_textures();
    trash_save();
    return 0;
}

static void trash_restore(int i)
{
    if (move_file(trash[i].file, trash[i].orig) < 0) {
        // Its folder is gone: back to the top of PICTURE instead.
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", plib_dir(), trash[i].name);
        if (move_file(trash[i].file, path) < 0) return;
    }
    memmove(&trash[i], &trash[i + 1], (trash_count - i - 1) * sizeof(Trashed));
    trash_count--;
    trash_forget_textures();
    trash_save();
}

// A small picture of a trashed photo, made when first drawn.
static const Texture *trash_thumb(int i)
{
    if (trash_tex_state[i] < 0) return NULL;
    if (trash_tex_state[i] > 0) return trash_tex[i];
    trash_tex_state[i] = -1;
    int w, h;
    u8 *px = image_load(trash[i].file, PTHUMB_H, &w, &h);
    if (!px || w <= 0 || h <= 0) { free(px); return NULL; }
    if (!trash_tex[i]) trash_tex[i] = tex_create(PTHUMB_W, PTHUMB_H);
    Texture *t = trash_tex[i];
    int cw = w, ch = h, ox = 0, oy = 0;
    if (w * PTHUMB_H > h * PTHUMB_W) { cw = h * PTHUMB_W / PTHUMB_H; ox = (w - cw) / 2; }
    else { ch = w * PTHUMB_H / PTHUMB_W; oy = (h - ch) / 2; }
    for (int y = 0; y < PTHUMB_H; y++)
        for (int x = 0; x < PTHUMB_W; x++) {
            const u8 *c = px + ((oy + y * ch / PTHUMB_H) * w + ox + x * cw / PTHUMB_W) * 4;
            t->px[y * t->tw + x] = RGBA(c[0], c[1], c[2], 255);
        }
    free(px);
    tex_upload(t);
    trash_tex_state[i] = 1;
    return t;
}

// After a delete or restore: read PICTURE again, keeping the place.
static void rescan(void)
{
    char name[64] = "";
    if (album >= 0) snprintf(name, sizeof(name), "%s", palbums[album].name);
    plib_scan();
    build_library();
    if (album >= 0) {
        album = -1;
        for (int i = 0; i < palbum_count; i++) if (!strcmp(palbums[i].name, name)) album = i;
    }
    int n = trash_open ? trash_count : album >= 0 ? palbums[album].count : item_count;
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    plib_thumbs(1);
}

// The photo square would delete here, or -1.
static int deletable(void)
{
    if (trash_open) return trash_count ? 0 : -1;
    if (album >= 0) return palbums[album].count ? palbums[album].first + sel : -1;
    if (view == VIEW_LIBRARY && item_count) return items[sel].photo;
    return -1;
}

static void do_confirm(void)
{
    if (confirm == 2) {
        trash_purge(sel);
        if (sel >= trash_count && sel > 0) sel--;
        return;
    }
    int photo = deletable();
    if (photo >= 0 && trash_put(photo) == 0) rescan();
}

// ---- Entry points -------------------------------------------------------------

void photos_enter(void)
{
    if (!f_head) {
        f_head = ui_font(FAMILY_SORA, 700, 18);
        f_small = ui_font(FAMILY_SORA, 400, 10);
        f_label = ui_font(FAMILY_SORA, 400, 9);
        f_tiny = ui_font(FAMILY_SORA, 700, 11);
        t_fill = tex_round_fill(8);
    }
    ScePspDateTime now;
    sceRtcGetCurrentClockLocalTime(&now);
    today = ymd_of(&now);
    plib_scan();
    build_library();
    trash_load();
    trash_open = 0;
    confirm = 0;
    album = -1;
    sel = album_sel = 0;
    scroll = 0;
    viewing = 0;
    play = -1;
    plib_thumbs(1);
}

void photos_leave(void) { plib_thumbs(0); }

int photos_viewing(void) { return viewing; }

int photos_take_play(void)
{
    int v = play;
    play = -1;
    return v;
}

int photos_update(const Input *in, float dt)
{
    if (viewing) {
        if (pviewer_update(in, dt)) {
            // Back on the photo that was showing.
            viewing = 0;
            int at = pviewer_current();
            if (view_from_library) {
                for (int k = 0; k < item_count; k++) if (items[k].photo == run[at]) sel = k;
            } else {
                sel = at;
            }
            plib_thumbs(1);
        }
        return 0;
    }
    u32 p = in->pressed;
    if (confirm) {
        if (p & PSP_CTRL_CROSS) { do_confirm(); confirm = 0; }
        else if (p & PSP_CTRL_CIRCLE) confirm = 0;
        return 0;
    }
    // Holding square 2 s asks to delete; a quick press switches Library and Albums.
    if (in->held & PSP_CTRL_SQUARE) {
        hold_t += dt;
        if (hold_t >= 2 && !hold_fired) {
            hold_fired = 1;
            if (deletable() >= 0) confirm = trash_open ? 2 : 1;
        }
        return 0;
    }
    if (hold_t > 0 && hold_t < 0.6f && album < 0 && !trash_open) {
        view = view == VIEW_LIBRARY ? VIEW_ALBUMS : VIEW_LIBRARY;
        sel = 0;
        scroll = 0;
    }
    hold_t = 0;
    hold_fired = 0;
    if (trash_open) {
        if (p & PSP_CTRL_CIRCLE) { trash_open = 0; return 0; }
        if (!trash_count) return 0;
        if (in->nav & PSP_CTRL_LEFT && sel > 0) sel--;
        if (in->nav & PSP_CTRL_RIGHT && sel < trash_count - 1) sel++;
        if (p & PSP_CTRL_CROSS) { trash_restore(sel); rescan(); }
        return 0;
    }
    if (p & PSP_CTRL_CIRCLE && album < 0) return 1;
    return view == VIEW_LIBRARY ? library_update(in) : albums_update(in);
}

// ---- Drawing ----------------------------------------------------------------

// The middle of a thumbnail, cut to w x h.
static void draw_crop(const Texture *t, float x, float y, float w, float h)
{
    float sw = t->w, sh = t->h;
    if (sw / sh > w / h) sw = sh * w / h; else sh = sw * h / w;
    gfx_filter(1);
    gfx_tex_part(t, (t->w - sw) / 2, (t->h - sh) / 2, sw, sh, x, y, w, h, 0xFFFFFFFF);
}

static void draw_placeholder(int seed, float x, float y, float w, float h)
{
    u32 c0, c1;
    ui_placeholder_colors(seed * 7 + 3, &c0, &c1);
    gfx_rect(x, y, w, h, color_lerp(c0, c1, 0.5f));
}

static void draw_tile(const Item *it, float x, float y, float s)
{
    const Texture *t = NULL;
    if (it->photo >= 0) { plib_want(it->photo); t = plib_thumb(it->photo); }
    else t = vlib_thumb(it->video);
    if (t) draw_crop(t, x, y, s, s);
    else draw_placeholder(it->photo >= 0 ? it->photo : it->video + 500, x, y, s, s);
    if (it->video >= 0) {
        const Video *v = &videos[it->video];
        char d[16] = "";
        if (v->duration_ms) ui_format_time(d, sizeof(d), v->duration_ms);
        float w = 14 + (*d ? font_width(f_label, d, 0) + 4 : 0);
        ui_rrect(x + s - w - 3, y + s - 14, w, 11, 3, RGBA(0, 0, 0, 170));
        ui_icon(ICON_PLAY, x + s - w + 1, y + s - 12, 7, TEXT);
        if (*d) font_draw(f_label, x + s - w + 11, y + s - 5, d, TEXT, 0);
    }
}

static void draw_pills(void)
{
    static const char *const names[] = { "Library", "Albums" };
    float x = 14;
    for (int i = 0; i < 2; i++) {
        float w = font_width(f_small, names[i], 0) + 18;
        if (i == view) gfx_nine(t_fill, 8, x, 32, w, 17, TEXT);
        else ui_ring(x, 32, w, 17, 8, CHIP);
        font_draw(f_small, x + 9, 44, names[i], i == view ? BG : MUTED, 0);
        x += w + 6;
    }
}

static void library_draw(float dt)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%d photo%s", photo_count, photo_count == 1 ? "" : "s");
    font_draw_right(f_small, SCREEN_W - 14, 44, buf, DIM);
    if (!item_count) return;
    // Keep the selected row whole on screen, its day's heading too when it fits.
    const Line *sl = &lines[item_line[sel]];
    float target = scroll, view_h = CONTENT_B - CONTENT_Y;
    if (sl->y - HEAD_H < target) target = sl->y - (sl > lines && sl[-1].head ? HEAD_H : 0);
    if (sl->y + LIB_STEP > target + view_h) target = sl->y + LIB_STEP - view_h;
    if (target < 0) target = 0;
    scroll = ui_approach(scroll, target, dt, 14);
    gfx_clip(0, CONTENT_Y - 2, SCREEN_W, CONTENT_B - CONTENT_Y + 2);
    char label[40];
    for (int l = 0; l < line_count; l++) {
        const Line *ln = &lines[l];
        float y = CONTENT_Y + ln->y - scroll;
        if (y > CONTENT_B || y + LIB_STEP < CONTENT_Y - 4) continue;
        if (ln->head) {
            day_label(ln->day, label, sizeof(label));
            font_draw(f_tiny, 14, y + 14, label, TEXT, 0);
            continue;
        }
        for (int k = 0; k < ln->n; k++) {
            int i = ln->first + k;
            float x = LIB_X + k * LIB_STEP;
            draw_tile(&items[i], x, y, LIB_TILE);
            if (i == sel) {
                gfx_rect(x - 1, y - 1, LIB_TILE + 2, 3, ACCENT);
                gfx_rect(x - 1, y + LIB_TILE - 2, LIB_TILE + 2, 3, ACCENT);
                gfx_rect(x - 1, y - 1, 3, LIB_TILE + 2, ACCENT);
                gfx_rect(x + LIB_TILE - 2, y - 1, 3, LIB_TILE + 2, ACCENT);
            }
        }
    }
    gfx_unclip();
    // A thin scroll bar for long libraries.
    float total = lines[line_count - 1].y + LIB_STEP;
    if (total > view_h) {
        float bh = view_h * view_h / total, by = CONTENT_Y + (view_h - bh) * scroll / (total - view_h);
        gfx_rect(SCREEN_W - 5, CONTENT_Y, 2, view_h, HEX(0x282A32, 255));
        gfx_rect(SCREEN_W - 5, by, 2, bh, MUTED);
    }
}

static void albums_draw(float dt)
{
    int n = palbum_count + (trash_count ? 1 : 0);
    char buf[48];
    snprintf(buf, sizeof(buf), "%d album%s", palbum_count, palbum_count == 1 ? "" : "s");
    font_draw_right(f_small, SCREEN_W - 14, 44, buf, DIM);
    int row = album_sel / 3;
    float target = row > 0 ? row - 1 : 0;
    scroll = ui_approach(scroll, target * CARD_STEP_Y, dt, 14);
    gfx_clip(0, CONTENT_Y - 4, SCREEN_W, CONTENT_B - CONTENT_Y + 4);
    for (int i = 0; i < n; i++) {
        float x = 14 + (i % 3) * CARD_STEP_X, y = CONTENT_Y + 4 + (i / 3) * CARD_STEP_Y - scroll;
        if (y > CONTENT_B || y + CARD_STEP_Y < CONTENT_Y) continue;
        int on = i == album_sel, is_trash = i == palbum_count;
        if (on) ui_rrect(x - 3, y - 3, CARD_W + 6, CARD_H + 6, 9, ACCENT);
        if (on) ui_rrect(x - 1, y - 1, CARD_W + 2, CARD_H + 2, 7, BG);
        const Texture *t;
        if (is_trash) t = trash_thumb(trash_count - 1);
        else { plib_want(palbums[i].first); t = plib_thumb(palbums[i].first); }
        if (t) draw_crop(t, x, y, CARD_W, CARD_H);
        else draw_placeholder(is_trash ? 99 : palbums[i].first, x, y, CARD_W, CARD_H);
        if (is_trash) gfx_rect(x, y, CARD_W, CARD_H, RGBA(14, 15, 19, 120));
        ui_round_corners(x, y, CARD_W, CARD_H, 6, BG);
        char count[16];
        snprintf(count, sizeof(count), "%d", is_trash ? trash_count : palbums[i].count);
        float cw = font_width(f_small, count, 0);
        font_draw_fit(f_small, x, y + CARD_H + 14, is_trash ? "Recently Deleted" : palbums[i].name, on ? TEXT : MUTED, CARD_W - cw - 8);
        font_draw_right(f_small, x + CARD_W, y + CARD_H + 14, count, DIM);
    }
    gfx_unclip();
}

static void album_draw(float dt)
{
    const PhotoAlbum *a = &palbums[album];
    char sub[32];
    snprintf(sub, sizeof(sub), "%d photo%s", a->count, a->count == 1 ? "" : "s");
    font_draw_fit(f_head, 14, 50, a->name, TEXT, 300);
    float hw = font_width(f_head, a->name, 0);
    font_draw(f_small, 14 + (hw < 300 ? hw : 300) + 8, 50, sub, MUTED, 0);
    int row = sel / COLS;
    float target = scroll;
    if (row < target) target = row;
    if (row > target + 1) target = row - 1;
    scroll = ui_approach(scroll, target, dt, 14);
    gfx_clip(0, 54, SCREEN_W, SCREEN_H - 54 - 22);
    for (int i = 0; i < a->count; i++) {
        float x = 14 + (i % COLS) * CELL_W, y = 62 + (i / COLS - scroll) * CELL_H;
        if (y > SCREEN_H || y + CELL_H < 40) continue;
        int photo = a->first + i, on = i == sel;
        if (on) {
            ui_rrect(x - 3, y - 3, PTHUMB_W + 6, PTHUMB_H + 6, 8, ACCENT);
            ui_rrect(x - 1, y - 1, PTHUMB_W + 2, PTHUMB_H + 2, 6, BG);
        }
        plib_want(photo);
        const Texture *t = plib_thumb(photo);
        if (t) { gfx_filter(1); gfx_tex(t, x, y, PTHUMB_W, PTHUMB_H, 0xFFFFFFFF); }
        else draw_placeholder(photo, x, y, PTHUMB_W, PTHUMB_H);
        font_draw_fit(f_label, x, y + PTHUMB_H + 13, photos[photo].name, on ? TEXT : MUTED, PTHUMB_W);
    }
    gfx_unclip();
}

static void trash_draw(void)
{
    font_draw(f_head, 14, 50, "Recently Deleted", TEXT, 0);
    font_draw(f_small, 14 + font_width(f_head, "Recently Deleted", 0) + 8, 50, "the last 4 deleted photos", MUTED, 0);
    if (!trash_count) { font_draw_center(f_small, SCREEN_W / 2, 140, "Nothing here", MUTED); return; }
    for (int i = 0; i < trash_count; i++) {
        float x = 14 + i * CELL_W, y = 62;
        int on = i == sel;
        if (on) {
            ui_rrect(x - 3, y - 3, PTHUMB_W + 6, PTHUMB_H + 6, 8, ACCENT);
            ui_rrect(x - 1, y - 1, PTHUMB_W + 2, PTHUMB_H + 2, 6, BG);
        }
        const Texture *t = trash_thumb(i);
        if (t) { gfx_filter(1); gfx_tex(t, x, y, PTHUMB_W, PTHUMB_H, 0xFFFFFFFF); }
        else draw_placeholder(i + 77, x, y, PTHUMB_W, PTHUMB_H);
        ui_round_corners(x, y, PTHUMB_W, PTHUMB_H, 5, BG);
        font_draw_fit(f_label, x, y + PTHUMB_H + 13, trash[i].name, on ? TEXT : MUTED, PTHUMB_W);
    }
}

// The bar that fills while square is held, and the question after it.
static void draw_delete_ui(void)
{
    if (hold_t > 0.3f && !hold_fired && deletable() >= 0) {
        float k = hold_t / 2 > 1 ? 1 : hold_t / 2, w = 220, x = (SCREEN_W - w) / 2, y = SCREEN_H - 56;
        ui_rrect(x, y, w, 30, 8, HEX(0x1A1C24, 240));
        font_draw_center(f_small, SCREEN_W / 2, y + 13, trash_open ? "Keep holding to delete for good" : "Keep holding to delete", TEXT);
        gfx_rect(x + 12, y + 20, w - 24, 3, HEX(0x2E3240, 255));
        gfx_rect(x + 12, y + 20, (w - 24) * k, 3, HEX(0xFF5B6B, 255));
    }
    if (!confirm) return;
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 150));
    float w = 260, h = 92, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    ui_rrect(x, y, w, h, 10, HEX(0x1A1C24, 255));
    ui_ring(x, y, w, h, 10, HEX(0x2E3240, 255));
    font_draw_center(f_tiny, SCREEN_W / 2, y + 26, confirm == 2 ? "Delete this photo for good?" : "Delete this photo?", TEXT);
    font_draw_center(f_small, SCREEN_W / 2, y + 44, confirm == 2 ? "This can't be undone." : "It stays in Recently Deleted for a while.", MUTED);
    float wx = ui_hint_width(f_label, ICON_CROSS, "Delete"), wo = ui_hint_width(f_label, ICON_CIRCLE, "Cancel");
    float hx = SCREEN_W / 2 - (wx + 18 + wo) / 2;
    ui_hint(f_label, hx, y + 74, ICON_CROSS, "Delete", TEXT);
    ui_hint(f_label, hx + wx + 18, y + 74, ICON_CIRCLE, "Cancel", MUTED);
}

static float hint(float x, int glyph, const char *label)
{
    x -= ui_hint_width(f_label, glyph, label);
    ui_hint(f_label, x, SCREEN_H - 8, glyph, label, MUTED);
    return x - 12;
}

void photos_draw(float dt)
{
    if (viewing) {
        pviewer_draw();
        return;
    }
    if (!photo_count && !item_count) {
        ui_empty_card(150, "No photos yet", "JPEG or PNG photos into", plib_dir(), "Each folder there becomes an album.");
        hint(SCREEN_W - 14, ICON_CIRCLE, "Back");
        return;
    }
    float x = SCREEN_W - 14;
    if (trash_open) {
        trash_draw();
        x = hint(x, ICON_CIRCLE, "Back");
        if (trash_count) {
            x = hint(x, ICON_SQUARE, "Hold: delete for good");
            hint(x, ICON_CROSS, "Restore");
        }
        draw_delete_ui();
        return;
    }
    if (album >= 0) {
        album_draw(dt);
        x = hint(x, ICON_CIRCLE, "Back");
        if (palbums[album].count) x = hint(x, ICON_SQUARE, "Hold: delete");
        hint(x, ICON_CROSS, "View");
        draw_delete_ui();
        return;
    }
    draw_pills();
    if (view == VIEW_LIBRARY) library_draw(dt);
    else albums_draw(dt);
    gfx_rect(0, SCREEN_H - 20, SCREEN_W, 20, BG);
    x = hint(x, ICON_CIRCLE, "Back");
    x = hint(x, ICON_SQUARE, view == VIEW_LIBRARY ? (item_count && items[sel].photo >= 0 ? "Albums (hold: delete)" : "Albums") : "Library");
    if (view == VIEW_LIBRARY && item_count) {
        x = hint(x, ICON_TRIANGLE, "Next month");
        hint(x, ICON_CROSS, items[sel].video >= 0 ? "Play" : "View");
    } else if (view == VIEW_ALBUMS && (palbum_count || trash_count)) {
        hint(x, ICON_CROSS, "Open");
    }
    draw_delete_ui();
}
