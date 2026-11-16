#include "gorder.h"

#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "glib.h"
#include "gfx.h"
#include "ui.h"

#define ACCENT HEX(0x5B8CFF, 255)
#define BG HEX(0x0E0F13, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define DIM HEX(0x8A8D99, 255)
#define CARD HEX(0x1E2026, 255)

#define ROWS 6
#define ROW_H 30

// Chstat bits: creation, access and modification times.
#define CST_TIMES (0x08 | 0x10 | 0x20)

typedef struct {
    int game;        // index into games, -1 for Boro itself
    char path[256];  // what the XMB dates: the ISO file or the EBOOT's folder
    u64 tick;        // its date now
} Entry;

static Entry *entries;
static int count, active, sel, picked, saving, saved_n;
static float scroll;

static void folder_of(const char *eboot, char *out, int size)
{
    snprintf(out, size, "%s", eboot);
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
}

static u64 date_of(const char *path)
{
    SceIoStat st;
    u64 tick = 0;
    if (sceIoGetstat(path, &st) >= 0) sceRtcGetTick(&st.sce_st_mtime, &tick);
    return tick;
}

static int cmp_newest(const void *a, const void *b)
{
    u64 x = ((const Entry *)a)->tick, y = ((const Entry *)b)->tick;
    return x < y ? 1 : x > y ? -1 : 0;
}

void gorder_open(void)
{
    free(entries);
    entries = malloc((game_count + 1) * sizeof(Entry));
    count = 0;
    if (!entries) return;
    for (int i = 0; i < game_count; i++) {
        Entry *e = &entries[count++];
        e->game = i;
        int len = strlen(games[i].path);
        if (len > 4 && !strcasecmp(games[i].path + len - 4, ".pbp")) folder_of(games[i].path, e->path, sizeof(e->path));
        else snprintf(e->path, sizeof(e->path), "%s", games[i].path);
        e->tick = date_of(e->path);
    }
    Entry *b = &entries[count++];
    b->game = -1;
    snprintf(b->path, sizeof(b->path), "%s", glib_app_dir());
    b->tick = date_of(b->path);
    qsort(entries, count, sizeof(Entry), cmp_newest);
    active = 1;
    sel = 0;
    picked = 0;
    saving = 0;
    scroll = 0;
}

int gorder_active(void) { return active; }

static const char *title_of(const Entry *e) { return e->game >= 0 ? games[e->game].title : "Boro"; }

static const char *kind_of(const Entry *e)
{
    if (e->game < 0) return "Boro";
    const Game *g = &games[e->game];
    int len = strlen(g->path);
    if (g->type == GTYPE_PS1) return "PS1";
    if (g->type == GTYPE_HOMEBREW) return "Homebrew";
    if (len > 4 && !strcasecmp(g->path + len - 4, ".cso")) return "CSO";
    if (len > 4 && !strcasecmp(g->path + len - 4, ".pbp")) return "EBOOT";
    return "ISO";
}

// One game per frame so the progress bar moves: the top of the list gets
// the newest date, a minute apart going down.
static void save_step(void)
{
    if (saved_n >= count) {
        saving = 0;
        active = 0;
        return;
    }
    u64 tick;
    sceRtcGetCurrentTick(&tick);
    tick -= (u64)saved_n * 60 * sceRtcGetTickResolution();
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    // sceRtcGetCurrentTick is UTC; the memory stick keeps local time.
    sceRtcConvertUtcToLocalTime(&tick, &tick);
    sceRtcSetTick(&st.sce_st_ctime, &tick);
    st.sce_st_atime = st.sce_st_ctime;
    st.sce_st_mtime = st.sce_st_ctime;
    sceIoChstat(entries[saved_n].path, &st, CST_TIMES);
    saved_n++;
}

int gorder_update(const Input *in, float dt)
{
    (void)dt;
    if (saving) {
        save_step();
        return active ? 0 : 2;
    }
    u32 p = in->pressed, nav = in->nav;
    if (p & PSP_CTRL_CIRCLE) {
        if (picked) picked = 0;
        else { active = 0; return 1; }
    }
    if (p & PSP_CTRL_CROSS) picked = !picked;
    if (nav & PSP_CTRL_UP && sel > 0) {
        if (picked) { Entry t = entries[sel]; entries[sel] = entries[sel - 1]; entries[sel - 1] = t; }
        sel--;
    }
    if (nav & PSP_CTRL_DOWN && sel < count - 1) {
        if (picked) { Entry t = entries[sel]; entries[sel] = entries[sel + 1]; entries[sel + 1] = t; }
        sel++;
    }
    if (p & PSP_CTRL_START) {
        picked = 0;
        saving = 1;
        saved_n = 0;
    }
    return 0;
}

static void draw_icon(const Entry *e, float x, float y, float w, float h)
{
    const Texture *t = e->game >= 0 ? glib_icon(e->game) : NULL;
    if (t) { gfx_filter(1); gfx_tex(t, x, y, w, h, 0xFFFFFFFF); }
    else gfx_rect(x, y, w, h, e->game < 0 ? color_alpha(ACCENT, 0.5f) : CARD);
}

void gorder_draw(float dt)
{
    Font *f_small = ui_font(FAMILY_SORA, 400, 10), *f_tiny = ui_font(FAMILY_SORA, 400, 9);
    Font *f_title = ui_font(FAMILY_SORA, 700, 18), *f_row = ui_font(FAMILY_SORA, 600, 11);
    Font *f_num = ui_font(FAMILY_SORA, 700, 10), *f_label = ui_font(FAMILY_SORA, 700, 8);
    gfx_begin(BG);
    font_draw(f_small, 14, 16, "Games", MUTED, 0);
    font_draw(f_title, 14, 36, "PSP menu order", TEXT, 0);
    ui_battery(f_small, SCREEN_W - 14, 12, MUTED);
    font_draw(f_small, 14, 52, "Top of the list shows first in the PSP menu", DIM, 0);

    float target = sel - ROWS / 2 + 1;
    if (target > count - ROWS) target = count - ROWS;
    if (target < 0) target = 0;
    scroll = ui_approach(scroll, target, dt, 14);
    gfx_clip(0, 60, 310, ROWS * ROW_H + 4);
    char num[8];
    for (int i = 0; i < count; i++) {
        float y = 64 + (i - scroll) * ROW_H;
        if (y < 40 || y > 60 + ROWS * ROW_H) continue;
        const Entry *e = &entries[i];
        int on = i == sel;
        if (on) {
            ui_rrect(10, y - 3, 290, 28, 6, picked ? HEX(0x22284A, 255) : HEX(0x1A1C24, 255));
            ui_ring(10, y - 3, 290, 28, 6, picked ? ACCENT : HEX(0x3A3D48, 255));
        }
        snprintf(num, sizeof(num), "%d", i + 1);
        font_draw(f_num, 16, y + 15, num, on ? ACCENT : DIM, 0);
        draw_icon(e, 40, y, 40, 22);
        font_draw_fit(f_row, 88, y + 10, title_of(e), TEXT, 180);
        font_draw(f_tiny, 88, y + 21, kind_of(e), MUTED, 0);
        if (on && picked) {
            ui_triangle(282, y + 3, UI_UP, ACCENT);
            ui_triangle(282, y + 14, UI_DOWN, ACCENT);
        }
    }
    gfx_unclip();

    // The first four, the way the PSP menu stacks them.
    ui_rrect(316, 64, 150, 176, 10, CARD);
    font_draw(f_label, 326, 78, "PSP MENU PREVIEW", MUTED, 0.5f);
    font_draw(f_tiny, 326, 92, "Game \xE2\x80\xBA Memory Stick", DIM, 0);
    for (int i = 0; i < 4 && i < count; i++) {
        float y = 100 + i * 34;
        draw_icon(&entries[i], 328, y, 52, 29);
        font_draw_fit(f_tiny, 388, y + 18, title_of(&entries[i]), i == sel ? TEXT : MUTED, 72);
    }

    if (saving) {
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(5, 6, 9, 200));
        ui_rrect(120, 86, 240, 100, 12, HEX(0x1A1C24, 250));
        ui_ring(120, 86, 240, 100, 12, HEX(0x2E3240, 255));
        font_draw(f_row, 140, 112, "Saving the order\xE2\x80\xA6", TEXT, 0);
        char line[64];
        snprintf(line, sizeof(line), "Setting dates on %d of %d games", saved_n, count);
        font_draw(f_small, 140, 130, line, MUTED, 0);
        gfx_rect(140, 146, 200, 3, RGBA(255, 255, 255, 40));
        gfx_rect(140, 146, 200.0f * saved_n / (count ? count : 1), 3, ACCENT);
        font_draw(f_tiny, 140, 170, "Nothing is moved or renamed.", DIM, 0);
    } else {
        float x = SCREEN_W - 14, base = SCREEN_H - 8;
        const char *cancel = picked ? "Put back" : "Cancel";
        x -= ui_hint_width(f_tiny, ICON_CIRCLE, cancel);
        ui_hint(f_tiny, x, base, ICON_CIRCLE, cancel, MUTED);
        x -= 12 + font_width(f_tiny, "START Save", 0);
        font_draw(f_tiny, x, base, "START Save", ACCENT, 0);
        x -= 12 + ui_hint_arrows_width(f_tiny, "Move");
        ui_hint_arrows(f_tiny, x, base, 1, "Move", MUTED);
        const char *pick = picked ? "Drop" : "Pick up";
        x -= 12 + ui_hint_width(f_tiny, ICON_CROSS, pick);
        ui_hint(f_tiny, x, base, ICON_CROSS, pick, MUTED);
    }
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
