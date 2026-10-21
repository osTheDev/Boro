#include "photos.h"

#include <pspctrl.h>
#include <stdio.h>
#include "font.h"
#include "gfx.h"
#include "plib.h"
#include "pviewer.h"
#include "ui.h"

#define BG HEX(0x0E0F13, 255)
#define ACCENT HEX(0x5B8CFF, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define STACK HEX(0x282A32, 255)
#define STACK_EDGE HEX(0x464852, 255)

#define COLS 4
#define GRID_X 14
#define GRID_Y 62
#define CELL_W (PTHUMB_W + 8)
#define CELL_H (PTHUMB_H + 24)
#define ROWS_SHOWN 2

static Font *f_head, *f_small, *f_label;
static int album = -1; // the open album, or -1 for the top level
static int sel, top_sel; // top_sel: where the selection was before opening the album
static float scroll;
static int viewing;

// Grid items: at the top level the palbums come first, then loose photos.
static int item_count(void)
{
    if (album >= 0) return palbums[album].count;
    return palbum_count + photo_count - loose_first;
}

static int item_photo(int i) // -1 for an album
{
    if (album >= 0) return palbums[album].first + i;
    return i < palbum_count ? -1 : loose_first + i - palbum_count;
}

void photos_enter(void)
{
    if (!f_head) {
        f_head = ui_font(FAMILY_SORA, 700, 18);
        f_small = ui_font(FAMILY_SORA, 400, 10);
        f_label = ui_font(FAMILY_SORA, 400, 9);
    }
    plib_scan();
    album = -1;
    sel = 0;
    scroll = 0;
    viewing = 0;
    plib_thumbs(1);
}

void photos_leave(void) { plib_thumbs(0); }

int photos_viewing(void) { return viewing; }

int photos_update(const Input *in, float dt)
{
    if (viewing) {
        if (pviewer_update(in, dt)) {
            // Back on the photo that was showing.
            viewing = 0;
            sel = pviewer_current() + (album < 0 ? palbum_count : 0);
            plib_thumbs(1);
        }
        return 0;
    }
    u32 p = in->pressed, nav = in->nav;
    if (p & PSP_CTRL_CIRCLE) {
        if (album < 0) return 1;
        album = -1;
        sel = top_sel;
        scroll = sel / COLS;
        return 0;
    }
    int n = item_count();
    if (!n) return 0;
    if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
    if (nav & PSP_CTRL_RIGHT && sel < n - 1) sel++;
    if (nav & PSP_CTRL_UP && sel >= COLS) sel -= COLS;
    if (nav & PSP_CTRL_DOWN && sel / COLS < (n - 1) / COLS) sel = sel + COLS < n ? sel + COLS : n - 1;
    int photo = item_photo(sel);
    if (p & PSP_CTRL_CROSS && photo < 0) {
        top_sel = sel;
        album = sel;
        sel = 0;
        scroll = 0;
    } else if (p & PSP_CTRL_CROSS) {
        // Flipping stays within the album, or within the loose photos.
        int first = album >= 0 ? palbums[album].first : loose_first;
        int count = album >= 0 ? palbums[album].count : photo_count - loose_first;
        plib_thumbs(0); // the viewer gets the CPU to itself
        pviewer_open(first, count, photo - first, album >= 0 ? palbums[album].name : "Photos");
        viewing = 1;
    }
    return 0;
}

static void draw_picture(int photo, float x, float y, float w, float h)
{
    plib_want(photo);
    const Texture *t = plib_thumb(photo);
    if (t) {
        gfx_filter(1);
        gfx_tex(t, x, y, w, h, 0xFFFFFFFF);
    } else {
        u32 c0, c1;
        ui_placeholder_colors(photo * 7 + 3, &c0, &c1);
        ui_rrect(x, y, w, h, 5, color_lerp(c0, c1, 0.5f));
    }
}

static void draw_item(int i, float x, float y)
{
    int photo = item_photo(i), on = i == sel;
    if (on) {
        ui_rrect(x - 3, y - 3, PTHUMB_W + 6, PTHUMB_H + 6, 8, ACCENT);
        ui_rrect(x - 1, y - 1, PTHUMB_W + 2, PTHUMB_H + 2, 6, BG);
    }
    float ly = y + PTHUMB_H + 13;
    if (photo >= 0) {
        draw_picture(photo, x, y, PTHUMB_W, PTHUMB_H);
        font_draw_fit(f_label, x, ly, photos[photo].name, on ? TEXT : MUTED, PTHUMB_W);
        return;
    }
    // An album: its first photo on a small stack of cards.
    const PhotoAlbum *a = &palbums[i];
    float w = PTHUMB_W - 6;
    for (int k = 2; k >= 1; k--) {
        ui_rrect(x + k * 3, y - k * 3, w, PTHUMB_H, 5, STACK_EDGE);
        ui_rrect(x + k * 3 + 1, y - k * 3 + 1, w - 2, PTHUMB_H - 2, 4, STACK);
    }
    draw_picture(a->first, x, y, w, PTHUMB_H);
    char count[16];
    snprintf(count, sizeof(count), "%d", a->count);
    float cw = font_width(f_label, count, 0);
    font_draw_right(f_label, x + w, ly, count, MUTED);
    font_draw_fit(f_label, x, ly, a->name, TEXT, w - cw - 6);
}

void photos_draw(float dt)
{
    if (viewing) {
        pviewer_draw();
        return;
    }
    int n = item_count();
    char sub[64];
    const char *head = "Photos";
    if (album >= 0) {
        head = palbums[album].name;
        snprintf(sub, sizeof(sub), "%d photo%s", n, n == 1 ? "" : "s");
    } else {
        int loose = photo_count - loose_first;
        if (palbum_count) snprintf(sub, sizeof(sub), "%d album%s \xC2\xB7 %d photo%s", palbum_count, palbum_count == 1 ? "" : "s", photo_count, photo_count == 1 ? "" : "s");
        else snprintf(sub, sizeof(sub), "%d photo%s", loose, loose == 1 ? "" : "s");
    }

    if (!n) {
        font_draw_center(f_head, SCREEN_W / 2, 120, "No photos yet", TEXT);
        font_draw_center(f_small, SCREEN_W / 2, 142, "Put JPEGs or PNGs in PICTURE on your memory stick.", MUTED);
        font_draw_center(f_small, SCREEN_W / 2, 156, "Each folder there becomes an album.", MUTED);
    } else {
        font_draw_fit(f_head, GRID_X, 50, head, TEXT, 300);
        float hw = font_width(f_head, head, 0);
        font_draw(f_small, GRID_X + (hw < 300 ? hw : 300) + 8, 50, sub, MUTED, 0);

        // Keep the selected row on screen, scrolling a row at a time.
        int row = sel / COLS;
        float target = scroll;
        if (row < target) target = row;
        if (row > target + ROWS_SHOWN - 1) target = row - ROWS_SHOWN + 1;
        scroll = ui_approach(scroll, target, dt, 14);
        gfx_clip(0, 54, SCREEN_W, SCREEN_H - 54 - 22);
        for (int i = 0; i < n; i++) {
            float y = GRID_Y + (i / COLS - scroll) * CELL_H;
            if (y > GRID_Y + (ROWS_SHOWN - 0.5f) * CELL_H || y + CELL_H < 40) continue;
            draw_item(i, GRID_X + (i % COLS) * CELL_W, y);
        }
        gfx_unclip();
    }

    float x = SCREEN_W - 14 - ui_hint_width(f_label, ICON_CIRCLE, "Back");
    ui_hint(f_label, x, SCREEN_H - 8, ICON_CIRCLE, "Back", MUTED);
    if (n) {
        const char *verb = item_photo(sel) < 0 ? "Open" : "View";
        x -= 12 + ui_hint_width(f_label, ICON_CROSS, verb);
        ui_hint(f_label, x, SCREEN_H - 8, ICON_CROSS, verb, MUTED);
    }
}
