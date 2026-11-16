#include "books.h"

#include <math.h>
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "blib.h"
#include "gfx.h"
#include "glib.h"
#include "player.h"
#include "settings.h"
#include "ui.h"

#define BG HEX(0x161311, 255)
#define CARD HEX(0x2C2622, 255)
#define WARM HEX(0xE8BE78, 255)
#define TEXT HEX(0xF4EEE6, 255)
#define MUTED HEX(0xA89C8E, 255)
#define DIM HEX(0x6E645A, 255)
#define RIBBON HEX(0xC8463C, 255)
#define TRACK_BG HEX(0x463E38, 255)
#define SHELF HEX(0x40322A, 255)

enum { V_SHELF, V_CARD, V_NOW, V_CONTENTS };
enum { TAB_SHELF, TAB_FINISHED };

static int inited, view = V_SHELF, contents_from = V_CARD;
static int tab, sort_title, sel, page = -1, button, c_sel, c_scroll;
static int list[1024], list_n;
static Font *f_head, *f_title, *f_label, *f_text, *f_small, *f_value, *f_button;
static Texture *t_glow;
static float t_now, grow = 1, shelf_x; // grow: the hovered book turning to its cover
static u32 spine_col[1024];             // from each cover, 0 until it loads

static void init(void)
{
    inited = 1;
    f_head = ui_font(FAMILY_SERIF, 700, 22);
    f_title = ui_font(FAMILY_SERIF, 700, 18);
    f_label = ui_font(FAMILY_SORA, 700, 8);
    f_text = ui_font(FAMILY_MANROPE, 500, 11);
    f_small = ui_font(FAMILY_MANROPE, 500, 10);
    f_value = ui_font(FAMILY_SORA, 700, 13);
    f_button = ui_font(FAMILY_MANROPE, 700, 11);
    t_glow = tex_glow(128);
}

// ---- Lists ------------------------------------------------------------------------

static int cmp_books(const void *a, const void *b)
{
    const Book *x = &books[*(const int *)a], *y = &books[*(const int *)b];
    if (!sort_title) {
        int c = strcmp(y->last, x->last); // latest first; never-opened ones last
        if (c) return c;
    }
    return strcasecmp(x->title, y->title);
}

static void build(void)
{
    list_n = 0;
    for (int i = 0; i < book_count && list_n < 1024; i++)
        if (books[i].finished == (tab == TAB_FINISHED)) list[list_n++] = i;
    qsort(list, list_n, sizeof(int), cmp_books);
    if (sel >= list_n) sel = list_n ? list_n - 1 : 0;
}

void books_enter(void)
{
    if (!inited) init();
    blib_scan();
    view = V_SHELF;
    sel = 0;
    shelf_x = 0;
    grow = 1;
    if (blib_ready()) build();
}

static void open_now(int book)
{
    page = book;
    view = V_NOW;
}

void books_enter_now(void)
{
    books_enter();
    int b = blib_current();
    if (b < 0) return;
    if (blib_playing() != b) blib_play(b);
    open_now(b);
}

// ---- Helpers ------------------------------------------------------------------------

static void format_len(char *out, int size, u32 ms)
{
    u32 m = (ms + 30000) / 60000;
    if (m >= 60) snprintf(out, size, "%uh %02um", (unsigned)(m / 60), (unsigned)(m % 60));
    else snprintf(out, size, "%um", (unsigned)m);
}

static float progress(int b)
{
    return books[b].total_ms ? fminf(1.0f, (float)blib_elapsed(b) / books[b].total_ms) : 0;
}

// The live position while this book plays, else the saved one.
static int now_track(int b, u32 *ms)
{
    int t = blib_playing() == b ? player_book_track() : -1;
    if (t >= 0) { *ms = player_position_ms(); return t; }
    *ms = books[b].pos_ms;
    return books[b].pos_track;
}

static void lamp(void)
{
    gfx_additive(1);
    gfx_tex(t_glow, 360 - 230, -150, 460, 300, HEX(0xFFBE78, 34));
    gfx_additive(0);
}

static void bar(float x, float y, float w, float p, u32 fill)
{
    ui_rrect(x, y, w, 3, 1.5f, TRACK_BG);
    if (p > 0) ui_rrect(x, y, fmaxf(3, w * p), 3, 1.5f, fill);
}

// The cover's own shape (width over height); 2:3 until it loads or without one.
static float cover_aspect(int b, int kind)
{
    const Texture *t = books[b].cover[0] ? art_get_file(books[b].cover, kind) : NULL;
    float a = t && t->h ? (float)t->w / t->h : 0.667f;
    return a < 0.4f ? 0.4f : a > 1.5f ? 1.5f : a;
}

// The biggest size of the whole cover that fits in max_w x max_h.
static void cover_fit(int b, int kind, float max_w, float max_h, float *w, float *h)
{
    float a = cover_aspect(b, kind);
    *h = max_h;
    *w = max_h * a;
    if (*w > max_w) { *w = max_w; *h = max_w / a; }
}

static void placeholder(int b, u32 *c0, u32 *c1) { ui_placeholder_colors(b * 7 + 3, c0, c1); }

static void cover(int b, float x, float y, float w, float h, int kind)
{
    const Book *bk = &books[b];
    const Texture *t = bk->cover[0] ? art_get_file(bk->cover, kind) : NULL;
    if (t) {
        gfx_filter(1);
        gfx_tex(t, x, y, w, h, 0xFFFFFFFF); // the whole cover, never cropped
    } else {
        u32 c0, c1;
        placeholder(b, &c0, &c1);
        ui_rrect(x, y, w, h, 3, c0);
        gfx_gradient(x, y + h * 0.5f, w, h * 0.5f, color_alpha(c1, 0), color_alpha(c1, 0), c1, c1);
        Font *f = w > 100 ? f_title : f_small;
        font_draw_fit(f, x + 6, y + h * 0.3f, bk->title, TEXT, w - 12);
    }
    if (!bk->finished && bk->last[0]) gfx_rect(x + w - 12, y, 6, h > 100 ? 26 : 16, RIBBON); // bookmark ribbon
}

// A soft, dim wash of the cover behind a page.
static void cover_wash(int b, float a)
{
    const Texture *t = books[b].cover[0] ? art_get_file(books[b].cover, ART_COVER_S) : NULL;
    if (t) {
        gfx_filter(1);
        gfx_tex_part(t, t->w * 0.2f, t->h * 0.2f, t->w * 0.6f, t->h * 0.6f, 0, 0, SCREEN_W, SCREEN_H, color_alpha(0xFFFFFFFF, a));
    } else {
        u32 c0, c1;
        placeholder(b, &c0, &c1);
        gfx_gradient(0, 0, SCREEN_W, SCREEN_H, color_alpha(c0, a), color_alpha(c1, a), color_alpha(c1, a), color_alpha(c0, a));
    }
    gfx_gradient(0, 0, SCREEN_W, SCREEN_H, HEX(0x161311, 150), HEX(0x161311, 110), HEX(0x161311, 220), HEX(0x161311, 200));
}

static void hints_bar(const int *glyph, const char *const *label, int n, float y)
{
    float x = SCREEN_W - 12;
    for (int i = n - 1; i >= 0; i--) {
        if (glyph[i] < 0) {
            x -= font_width(f_small, label[i], 0);
            font_draw(f_small, x, y, label[i], MUTED, 0);
        } else {
            x -= ui_hint_width(f_small, glyph[i], label[i]);
            ui_hint(f_small, x, y, glyph[i], label[i], MUTED);
        }
        x -= 12;
    }
}

static void pill(float x, float y, const char *label, int on, int filled)
{
    float w = font_width(f_button, label, 0) + 28;
    if (filled) ui_rrect(x, y, w, 24, 12, on ? WARM : CARD);
    else {
        ui_rrect(x, y, w, 24, 12, on ? HEX(0x4A3E34, 255) : CARD);
        if (on) ui_ring(x, y, w, 24, 12, WARM);
    }
    font_draw(f_button, x + 14, y + 16, label, filled && on ? BG : TEXT, 0);
}

static float pill_w(const char *label) { return font_width(f_button, label, 0) + 28; }

// ---- Shelf --------------------------------------------------------------------------

#define FLOOR 226
#define COVER_H 114

static float hold_t, toast_t; // square held; "Starting over" note
static int hold_fired;

static int restartable(void)
{
    if (!list_n) return -1;
    int b = list[sel];
    return books[b].finished || books[b].last[0] || books[b].pos_track || books[b].pos_ms ? b : -1;
}

// Back to the shelf: the book stops (paused, place saved).
static void pause_book(void)
{
    if (blib_playing() >= 0 && player_book_track() >= 0 && !player_paused()) player_toggle_pause();
    blib_save();
}

static void shelf_update(u32 p, u32 nav, u32 held, float dt)
{
    if (!blib_ready()) return;
    int old = sel;
    // Holding square 2 s starts the book over; a quick press changes the sort.
    if (held & PSP_CTRL_SQUARE) {
        hold_t += dt;
        if (hold_t >= 2 && !hold_fired) {
            hold_fired = 1;
            int b = restartable();
            if (b >= 0) {
                blib_restart(b);
                if (blib_playing() == b) pause_book();
                toast_t = 1.6f;
                build();
                for (int i = 0; i < list_n; i++) if (list[i] == b) sel = i;
                grow = 0;
            }
        }
        return;
    }
    if (hold_t > 0 && hold_t < 0.6f) { sort_title ^= 1; build(); }
    hold_t = 0;
    hold_fired = 0;
    if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) { tab ^= 1; sel = 0; shelf_x = 0; build(); }
    if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
    if (nav & PSP_CTRL_RIGHT && sel < list_n - 1) sel++;
    if (sel != old) grow = 0;
    int current = blib_current();
    if (p & PSP_CTRL_TRIANGLE && current >= 0) {
        if (blib_playing() != current) blib_play(current);
        open_now(current);
    }
    if (p & PSP_CTRL_CROSS && list_n) { page = list[sel]; button = 0; view = V_CARD; }
}

// Each book's spine: a steady size from its place, a color from its cover.
static void spine_size(int b, float *w, float *h)
{
    u32 k = (u32)b * 2654435761u;
    *w = 16 + (k >> 8) % 5 * 2;
    *h = 88 + (k >> 16) % 5 * 6;
}

static u32 spine_color(int b)
{
    if (b < 1024 && spine_col[b]) return spine_col[b];
    const Texture *t = books[b].cover[0] ? art_get_file(books[b].cover, ART_COVER_S) : NULL;
    if (t && t->px && !t->alpha8) {
        u32 r = 0, g = 0, bl = 0, n = 0;
        for (int y = t->h / 8; y < t->h; y += t->h / 4 + 1)
            for (int x = t->w / 8; x < t->w; x += t->w / 4 + 1) {
                u32 c = t->px[y * t->tw + x];
                r += c & 0xFF; g += (c >> 8) & 0xFF; bl += (c >> 16) & 0xFF; n++;
            }
        if (n) {
            u32 c = RGBA(r / n * 4 / 5, g / n * 4 / 5, bl / n * 4 / 5, 255);
            if (b < 1024) spine_col[b] = c;
            return c;
        }
    }
    u32 c0, c1;
    placeholder(b, &c0, &c1);
    return c0;
}

static void spine(int b, float x, float w, float h)
{
    u32 c = spine_color(b), y = FLOOR - h;
    ui_rrect(x, y, w, h, 2, c);
    gfx_rect(x + w - 3, y + 1, 3, h - 1, color_lerp(c, BG, 0.4f));
    u32 band = color_lerp(c, TEXT, 0.3f);
    gfx_rect(x + 2, y + 10, w - 6, 2, band);
    gfx_rect(x + 2, FLOOR - 14, w - 6, 2, band);
    if (!books[b].finished && books[b].last[0]) gfx_rect(x + w / 2 - 2, y - 6, 4, 12, RIBBON);
}

static void shelf_draw(void)
{
    font_draw(f_head, 16, 28, "Library", TEXT, 0);
    float tx = 110;
    static const char *const tabs[2] = { "Shelf", "Finished" };
    for (int i = 0; i < 2; i++) {
        font_draw(f_text, tx, 26, tabs[i], i == tab ? TEXT : DIM, 0);
        float tw = font_width(f_text, tabs[i], 0);
        if (i == tab) ui_rrect(tx, 30, tw, 2, 1, WARM);
        tx += tw + 14;
    }
    ui_battery(f_small, SCREEN_W - 12, 18, MUTED);
    if (!blib_ready()) {
        font_draw_center(f_title, SCREEN_W / 2, 140, "Dusting off the shelves\xE2\x80\xA6", TEXT);
        return;
    }
    if (!book_count) {
        ui_rrect(70, 76, 340, 110, 12, CARD);
        font_draw_center(f_head, SCREEN_W / 2, 114, "No books yet", TEXT);
        char line[128];
        snprintf(line, sizeof(line), "Copy each book into its own folder in %s/", blib_dir());
        font_draw_center(f_text, SCREEN_W / 2, 140, line, MUTED);
        font_draw_center(f_small, SCREEN_W / 2, 158, "MP3 or M4B. Files play in name order. Add cover.jpg for the art.", DIM);
        return;
    }
    if (!list_n) {
        font_draw_center(f_text, SCREEN_W / 2, 140, tab == TAB_FINISHED ? "Finished books will rest here." : "Every book here is finished.", MUTED);
    } else {
        // About the book you're on.
        int b = list[sel];
        const Book *bk = &books[b];
        const char *label = bk->finished ? "FINISHED" : bk->last[0] ? (b == blib_current() ? "CONTINUE" : "IN PROGRESS") : "NEXT UP";
        font_draw(f_label, 16, 48, label, WARM, 1);
        font_draw_fit(f_title, 16, 68, bk->title, TEXT, 448);
        char line[200], len[32];
        int t = bk->finished ? 0 : bk->pos_track;
        snprintf(line, sizeof(line), "%s \xC2\xB7 Ch %d of %d", bk->author[0] ? bk->author : "Unknown author", t + 1, bk->track_count);
        font_draw_fit(f_text, 16, 84, line, MUTED, 448);
        float p = progress(b);
        bar(16, 93, 170, p, WARM);
        format_len(len, sizeof(len), bk->finished ? bk->total_ms : bk->total_ms - blib_elapsed(b));
        if (bk->finished) snprintf(line, sizeof(line), "Finished \xC2\xB7 %s", len);
        else snprintf(line, sizeof(line), "%d%% \xC2\xB7 %s left", (int)(p * 100), len);
        font_draw(f_small, 194, 98, line, MUTED, 0);

        // The shelf: spines, with the book you're on grown into its cover.
        float k = grow * grow * (3 - 2 * grow);
        float cw = cover_aspect(b, ART_COVER_S) * COVER_H, sw, sh;
        // Scroll so the book you're on stays in view.
        float x = 0, at = 0;
        for (int i = 0; i < list_n; i++) {
            spine_size(list[i], &sw, &sh);
            if (i == sel) { at = x; x += sw + (cw - sw) * k + 6; }
            else x += sw + 3;
        }
        float want = shelf_x;
        if (at - want < 0) want = at;
        if (at - want + cw + 32 > SCREEN_W - 16) want = at + cw + 32 - (SCREEN_W - 16);
        shelf_x += (want - shelf_x) * 0.25f;
        x = 16 - shelf_x;
        for (int i = 0; i < list_n && x < SCREEN_W; i++) {
            int bi = list[i];
            spine_size(bi, &sw, &sh);
            if (i == sel) {
                float w = sw + (cw - sw) * k, h = sh + (COVER_H - sh) * k;
                if (x + w > 0) {
                    ui_shadow(x + 4, FLOOR - h + 6, w, h, 4, 8, RGBA(0, 0, 0, 150));
                    cover(bi, x, FLOOR - h, w, h, ART_COVER_S);
                    ui_ring(x - 2, FLOOR - h - 2, w + 4, h + 4, 4, color_alpha(WARM, k));
                }
                x += w + 6;
            } else {
                if (x + sw > 0) spine(bi, x, sw, sh);
                x += sw + 3;
            }
        }
    }
    ui_rrect(8, FLOOR, SCREEN_W - 16, 6, 2, SHELF);
    gfx_rect(8, FLOOR + 6, SCREEN_W - 16, 3, HEX(0x1E1814, 255));
    int current = blib_current();
    static const int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_SQUARE, -1, ICON_CIRCLE };
    const char *l[] = { "Open", "Continue", "Sort, hold: Restart", "L/R Shelf", "Back" };
    if (current >= 0) hints_bar(g, l, 5, 264);
    else {
        static const int g2[] = { ICON_CROSS, ICON_SQUARE, -1, ICON_CIRCLE };
        const char *l2[] = { "Open", l[2], "L/R Shelf", "Back" };
        hints_bar(g2, l2, 4, 264);
    }
    // Hold square: a bar fills, then the book starts over.
    float bw = 220, bx = SCREEN_W - 16 - bw, by = 40;
    if (hold_t > 0.3f && !hold_fired && restartable() >= 0) {
        float k = hold_t / 2 > 1 ? 1 : hold_t / 2;
        ui_rrect(bx, by, bw, 30, 8, HEX(0x2C2622, 245));
        font_draw_center(f_small, bx + bw / 2, by + 13, "Keep holding to start over", TEXT);
        ui_rrect(bx + 12, by + 20, bw - 24, 3, 1, TRACK_BG);
        ui_rrect(bx + 12, by + 20, (bw - 24) * k, 3, 1, WARM);
    } else if (toast_t > 0) {
        toast_t -= 1.0f / 60;
        float a = toast_t > 1 ? 1 : toast_t;
        ui_rrect(bx, by, bw, 24, 8, color_alpha(HEX(0x2C2622, 245), a));
        font_draw_center(f_small, bx + bw / 2, by + 16, "Back to the beginning", color_alpha(TEXT, a));
    }
}

// ---- Catalog card ---------------------------------------------------------------------

static void card_update(u32 p)
{
    if (p & PSP_CTRL_LEFT && button > 0) button--;
    if (p & PSP_CTRL_RIGHT && button < 2) button++;
    if (p & PSP_CTRL_TRIANGLE) button = 1, p |= PSP_CTRL_CROSS;
    if (p & PSP_CTRL_CIRCLE) { pause_book(); view = V_SHELF; build(); return; }
    if (!(p & PSP_CTRL_CROSS)) return;
    if (button == 0) {
        if (blib_playing() != page) blib_play(page);
        else if (player_paused()) player_toggle_pause();
        open_now(page);
    } else if (button == 1) {
        u32 ms;
        c_sel = books[page].finished ? 0 : now_track(page, &ms);
        c_scroll = c_sel > 3 ? c_sel - 3 : 0;
        contents_from = V_CARD;
        view = V_CONTENTS;
    } else {
        blib_restart(page);
        button = 0;
    }
}

static void card_draw(void)
{
    const Book *b = &books[page];
    cover_wash(page, 0.5f);
    char crumb[160];
    snprintf(crumb, sizeof(crumb), "Library \xE2\x80\xBA %s", b->title);
    font_draw_fit(f_small, 16, 20, crumb, MUTED, 360);
    ui_battery(f_small, SCREEN_W - 12, 16, MUTED);
    float cw, ch;
    cover_fit(page, ART_COVER_L, 112, 168, &cw, &ch);
    float cx = 16 + (112 - cw) / 2;
    ui_shadow(cx + 4, 40, cw, ch, 6, 10, RGBA(0, 0, 0, 170));
    cover(page, cx, 34, cw, ch, ART_COVER_L);

    float x = 148, w = SCREEN_W - 16 - x;
    font_draw_fit(f_head, x, 58, b->title, TEXT, w);
    font_draw_fit(f_text, x, 76, b->author[0] ? b->author : "Unknown author", MUTED, w);

    char v[4][40], len[32], left[32];
    format_len(len, sizeof(len), b->total_ms);
    snprintf(v[0], sizeof(v[0]), "%s", len);
    snprintf(v[1], sizeof(v[1]), "%d", b->track_count);
    if (b->finished) snprintf(v[2], sizeof(v[2]), "Done");
    else snprintf(v[2], sizeof(v[2]), "%d%%", (int)(progress(page) * 100));
    if (b->last[0]) glib_format_last(v[3], sizeof(v[3]), b->last);
    else snprintf(v[3], sizeof(v[3]), "Never");
    static const char *const keys[4] = { "LENGTH", "CHAPTERS", "PROGRESS", "LAST" };
    for (int i = 0; i < 4; i++) {
        float bx = x + i * 80;
        ui_rrect(bx, 92, 74, 36, 8, HEX(0x2C2622, 220));
        font_draw(f_label, bx + 8, 104, keys[i], DIM, 1);
        font_draw_fit(f_value, bx + 8, 122, v[i], TEXT, 62);
    }
    bar(x, 142, w, b->finished ? 1 : progress(page), WARM);
    format_len(left, sizeof(left), b->total_ms - blib_elapsed(page));
    const char *cv = !b->cover[0] ? "no cover" : strstr(b->cover, "/bookart/") ? "cover from the file" : strrchr(b->cover, '/') + 1;
    const char *fmt = b->tracks[0].kind == BOOK_M4B ? "M4B" : "MP3";
    char line[160];
    if (b->finished) snprintf(line, sizeof(line), "Finished \xC2\xB7 %s \xC2\xB7 %s", fmt, cv);
    else snprintf(line, sizeof(line), "%s left \xC2\xB7 %s \xC2\xB7 %s", left, fmt, cv);
    font_draw_fit(f_small, x, 158, line, MUTED, w);

    const char *labels[3] = { b->finished ? "Listen again" : b->last[0] ? "Continue" : "Start listening", "Contents", "Start over" };
    float bx = x;
    for (int i = 0; i < 3; i++) {
        pill(bx, 176, labels[i], i == button, 1);
        bx += pill_w(labels[i]) + 8;
    }
    static const int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE };
    static const char *const l[] = { "Select", "Contents", "Back" };
    hints_bar(g, l, 3, 264);
}

// ---- Now listening ---------------------------------------------------------------------

static void skip(int b, int seconds)
{
    if (blib_playing() != b) return;
    int t = player_book_track();
    if (t < 0) return;
    s32 at = (s32)player_position_ms() + seconds * 1000;
    u32 len = books[b].tracks[t].duration_ms;
    if (at < 0) {
        if (t > 0) { player_jump(t - 1); return; } // jump() starts it from the top
        at = 0;
    }
    if (len && (u32)at >= len) {
        if (t + 1 < books[b].track_count) player_jump(t + 1);
        return;
    }
    player_seek((u32)at);
}

static void now_update(u32 p, u32 nav)
{
    int b = page, mine = blib_playing() == b && player_book_track() >= 0;
    if (p & PSP_CTRL_CIRCLE) { view = V_CARD; button = 0; return; }
    if (p & PSP_CTRL_CROSS) {
        if (!mine) blib_play(b);
        else {
            player_toggle_pause();
            if (player_paused()) blib_save();
        }
    }
    if (p & PSP_CTRL_TRIANGLE) {
        u32 ms;
        c_sel = now_track(b, &ms);
        c_scroll = c_sel > 3 ? c_sel - 3 : 0;
        contents_from = V_NOW;
        view = V_CONTENTS;
        return;
    }
    if (p & PSP_CTRL_SQUARE) {
        settings.book_speed = (settings.book_speed + 1) % BOOK_SPEEDS;
        settings_save();
        player_set_speed(book_speeds[settings.book_speed]);
    }
    if (p & PSP_CTRL_START) settings_sleep_cycle();
    if (!mine) return;
    if (nav & PSP_CTRL_LEFT) skip(b, -30);
    if (nav & PSP_CTRL_RIGHT) skip(b, 30);
    int t = player_book_track();
    if (p & PSP_CTRL_LTRIGGER) player_jump(player_position_ms() > 3000 || t == 0 ? t : t - 1);
    if (p & PSP_CTRL_RTRIGGER && t + 1 < books[b].track_count) player_jump(t + 1);
}

static void now_draw(void)
{
    int b = page, mine = blib_playing() == b && player_book_track() >= 0;
    const Book *bk = &books[b];
    cover_wash(b, 0.45f);
    font_draw(f_small, 16, 20, "Now listening", MUTED, 0);
    ui_battery(f_small, SCREEN_W - 12, 16, MUTED);
    float cw, ch;
    cover_fit(b, ART_COVER_L, 128, 184, &cw, &ch);
    float cx = 16 + (128 - cw) / 2, cy = 36 + (184 - ch) / 2;
    ui_shadow(cx + 4, cy + 6, cw, ch, 6, 10, RGBA(0, 0, 0, 180));
    cover(b, cx, cy, cw, ch, ART_COVER_L);

    u32 ms;
    int t = now_track(b, &ms);
    float x = 164, w = SCREEN_W - 16 - x;
    char label[40];
    snprintf(label, sizeof(label), "CHAPTER %d OF %d", t + 1, bk->track_count);
    font_draw(f_label, x, 46, label, WARM, 1);
    font_draw_fit(f_title, x, 66, bk->names[t], TEXT, w);
    char sub[200];
    snprintf(sub, sizeof(sub), "%s%s%s", bk->title, bk->author[0] ? " \xC2\xB7 " : "", bk->author);
    font_draw_fit(f_text, x, 82, sub, MUTED, w);
    u32 len = bk->tracks[t].duration_ms;
    bar(x, 98, w, len ? fminf(1.0f, (float)ms / len) : 0, TEXT);
    char pos[16], rest[20];
    ui_format_time(pos, sizeof(pos), ms);
    rest[0] = '-';
    ui_format_time(rest + 1, sizeof(rest) - 1, len > ms ? len - ms : 0);
    font_draw(f_small, x, 114, pos, MUTED, 0);
    font_draw_right(f_small, x + w, 114, rest, MUTED);

    float mx = x + w / 2, my = 146;
    int playing_now = mine && !player_paused();
    for (int s = -1; s <= 1; s += 2) {
        float rx = mx + s * 104;
        ui_ring(rx - 12, my - 12, 24, 24, 12, MUTED);
        font_draw_center(f_label, rx, my + 3, "30", TEXT);
        ui_triangle(rx + s * 10 - (s < 0 ? 4 : 0), my - 13, s < 0 ? UI_LEFT : UI_RIGHT, MUTED);
    }
    ui_icon(ICON_PREV, mx - 60, my - 8, 16, TEXT);
    ui_icon(ICON_NEXT, mx + 44, my - 8, 16, TEXT);
    ui_rrect(mx - 18, my - 18, 36, 36, 18, TEXT);
    ui_icon(playing_now ? ICON_PAUSE : ICON_PLAY, mx - 8, my - 8, 16, BG);
    if (!mine) font_draw_center(f_small, mx, my + 32, bk->last[0] ? "Paused here \xC2\xB7 press X to listen" : "Press X to start listening", MUTED);

    // Speed, sleep and chapters chips.
    char sp[32], sl[32];
    float v = book_speeds[settings.book_speed];
    if (v == (int)v) snprintf(sp, sizeof(sp), "%d\xC3\x97", (int)v);
    else snprintf(sp, sizeof(sp), "%g\xC3\x97", v);
    int sleep_min = settings_sleep_left();
    if (sleep_min) snprintf(sl, sizeof(sl), "Sleep %dm", sleep_min);
    else snprintf(sl, sizeof(sl), "Sleep off");
    float px = x;
    pill(px, 186, sp, settings.book_speed != 0, 0);
    px += pill_w(sp) + 8;
    pill(px, 186, sl, sleep_min != 0, 0);
    px += pill_w(sl) + 8;
    pill(px, 186, "Chapters", 0, 0);

    float bp = bk->total_ms ? fminf(1.0f, (float)blib_elapsed(b) / bk->total_ms) : 0;
    if (mine && bk->total_ms) {
        u32 done = ms;
        for (int i = 0; i < t; i++) done += bk->tracks[i].duration_ms;
        bp = fminf(1.0f, (float)done / bk->total_ms);
    }
    bar(x, 222, w, bp, WARM);
    char line[64], left[32];
    format_len(left, sizeof(left), (u32)(bk->total_ms * (1 - bp)));
    snprintf(line, sizeof(line), "%d%% of book \xC2\xB7 %s left", (int)(bp * 100), left);
    font_draw(f_small, x, 238, line, MUTED, 0);

    static const int g[] = { -1, -1, ICON_TRIANGLE, ICON_SQUARE, -1, ICON_CROSS, ICON_CIRCLE };
    const char *l[] = { "\xE2\x97\x80\xE2\x96\xB6 30s", "L/R Chapter", "Chapters", "Speed", "START Sleep", playing_now ? "Pause" : "Play", "Back" };
    hints_bar(g, l, 7, 262);
}

// ---- Contents -----------------------------------------------------------------------------

static void contents_update(u32 p, u32 nav)
{
    const Book *bk = &books[page];
    if (p & PSP_CTRL_CIRCLE) { view = contents_from; return; }
    if (nav & PSP_CTRL_UP && c_sel > 0) c_sel--;
    if (nav & PSP_CTRL_DOWN && c_sel < bk->track_count - 1) c_sel++;
    if (nav & PSP_CTRL_LTRIGGER) c_sel = c_sel > 7 ? c_sel - 7 : 0;
    if (nav & PSP_CTRL_RTRIGGER) c_sel = c_sel + 7 < bk->track_count ? c_sel + 7 : bk->track_count - 1;
    if (c_sel < c_scroll) c_scroll = c_sel;
    if (c_sel > c_scroll + 6) c_scroll = c_sel - 6;
    if (p & PSP_CTRL_CROSS) {
        blib_play_track(page, c_sel);
        open_now(page);
    }
}

static void contents_draw(void)
{
    const Book *bk = &books[page];
    cover_wash(page, 0.3f);
    ui_battery(f_small, SCREEN_W - 12, 16, MUTED);
    font_draw(f_head, 16, 30, "Chapters", TEXT, 0);
    font_draw_fit(f_text, 16 + font_width(f_head, "Chapters", 0) + 12, 28, bk->title, MUTED, 300);
    u32 ms;
    int cur = now_track(page, &ms);
    for (int i = c_scroll; i < bk->track_count && i < c_scroll + 7; i++) {
        float y = 42 + (i - c_scroll) * 28;
        int on = i == c_sel, done = bk->finished || i < cur;
        if (on) ui_rrect(12, y, SCREEN_W - 24, 26, 8, HEX(0x2C2622, 235));
        char num[8], len[16];
        snprintf(num, sizeof(num), "%d", i + 1);
        font_draw_right(f_value, 44, y + 18, num, i == cur ? WARM : DIM);
        font_draw_fit(on ? f_button : f_text, 56, y + 17, bk->names[i], done && !on ? MUTED : TEXT, 330);
        ui_format_time(len, sizeof(len), bk->tracks[i].duration_ms);
        font_draw_right(f_small, SCREEN_W - 44, y + 17, len, MUTED);
        if (i == cur && !bk->finished) bar(56, y + 21, 330, bk->tracks[i].duration_ms ? fminf(1.0f, (float)ms / bk->tracks[i].duration_ms) : 0, WARM);
        if (done && i != cur) {
            float cx = SCREEN_W - 32, cy = y + 13;
            for (int k = 0; k < 3; k++) gfx_rect(cx + k, cy + k - 1, 2, 2, WARM);
            for (int k = 0; k < 5; k++) gfx_rect(cx + 3 + k, cy + 1 - k, 2, 2, WARM);
        }
    }
    if (c_scroll + 7 < bk->track_count) font_draw_center(f_small, SCREEN_W / 2, 244, "\xE2\x80\xA6", MUTED);
    static const int g[] = { ICON_CROSS, -1, ICON_CIRCLE };
    static const char *const l[] = { "Play chapter", "L/R Page", "Back" };
    hints_bar(g, l, 3, 264);
}

// ---- Main ------------------------------------------------------------------------------------

int books_update(const Input *in, float dt)
{
    t_now += dt;
    grow = fminf(1.0f, grow + dt * 5);
    static int was_ready;
    if (blib_ready() && !was_ready) build();
    was_ready = blib_ready();
    u32 p = in->pressed, nav = in->nav;
    if (view != V_SHELF && (page < 0 || page >= book_count)) view = V_SHELF;
    switch (view) {
    case V_SHELF:
        if (p & PSP_CTRL_CIRCLE) return 1;
        shelf_update(p, nav, in->held, dt);
        break;
    case V_CARD: card_update(p); break;
    case V_NOW: now_update(p, nav); break;
    case V_CONTENTS: contents_update(p, nav); break;
    }
    return 0;
}

void books_draw(float dt)
{
    (void)dt;
    gfx_begin(BG);
    lamp();
    switch (view) {
    case V_SHELF: shelf_draw(); break;
    case V_CARD: card_draw(); break;
    case V_NOW: now_draw(); break;
    case V_CONTENTS: contents_draw(); break;
    }
    ui_volume(f_small, HEX(0x2C2622, 240), TRACK_BG, WARM, TEXT);
    gfx_end();
}
