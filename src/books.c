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
#include "ui.h"

#define WOOD HEX(0x3A2418, 255)
#define WOOD2 HEX(0x543622, 255)
#define DARKW HEX(0x24160F, 255)
#define PAPER HEX(0xF0E6D0, 255)
#define PAPER2 HEX(0xE2D4B8, 255)
#define INK HEX(0x34261C, 255)
#define FADE HEX(0x826E5A, 255)
#define BRASS HEX(0xC8A050, 255)
#define RED HEX(0x962828, 255)
#define RULE HEX(0xCDBEA5, 255)
#define TRACK_BG HEX(0xD2C4AA, 255)

enum { V_SHELF, V_CARD, V_NOW, V_CONTENTS };
enum { TAB_SHELF, TAB_FINISHED };

static int inited, view = V_SHELF, contents_from = V_CARD;
static int tab, sort_title, sel, scroll, page = -1, button, c_sel, c_scroll;
static int list[1024], list_n;
static Font *f_head, *f_title, *f_bold, *f_text, *f_small, *f_tiny;
static Texture *t_glow;
static u32 grain[96];
static float t_now;

static void init(void)
{
    inited = 1;
    f_head = ui_font(FAMILY_SERIF, 700, 22);
    f_title = ui_font(FAMILY_SERIF, 700, 15);
    f_bold = ui_font(FAMILY_SERIF, 700, 12);
    f_text = ui_font(FAMILY_SERIF, 400, 12);
    f_small = ui_font(FAMILY_SERIF, 400, 11);
    f_tiny = ui_font(FAMILY_SERIF, 400, 10);
    t_glow = tex_glow(128);
    u32 seed = 7;
    for (int i = 0; i < 96; i++) {
        seed = seed * 1103515245u + 12345u;
        int v = (int)((seed >> 16) % 13) - 6;
        grain[i] = RGBA(0x3A + v, 0x24 + v, 0x18 + v, 255);
    }
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
    int current = tab == TAB_SHELF ? blib_current() : -1;
    for (int i = 0; i < book_count && list_n < 1024; i++)
        if (books[i].finished == (tab == TAB_FINISHED) && i != current) list[list_n++] = i;
    qsort(list, list_n, sizeof(int), cmp_books);
    if (sel >= list_n) sel = list_n ? list_n - 1 : 0;
}

void books_enter(void)
{
    if (!inited) init();
    blib_scan();
    view = V_SHELF;
    sel = scroll = 0;
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

static void roman(char *out, int size, int n)
{
    static const int val[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char *const sym[] = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
    out[0] = 0;
    for (int i = 0; i < 13 && n > 0; i++)
        while (n >= val[i] && (int)strlen(out) + 3 < size) { strcat(out, sym[i]); n -= val[i]; }
}

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

static void wood(void)
{
    for (int i = 0; i < 96; i++) gfx_rect(0, i * 3, SCREEN_W, 3, grain[i]);
    gfx_additive(1);
    gfx_tex(t_glow, 380 - 260, -180, 520, 360, HEX(0xFFD68C, 40));
    gfx_additive(0);
}

static void bar(float x, float y, float w, float p, u32 fill, u32 bg)
{
    gfx_rect(x, y, w, 3, bg);
    gfx_rect(x, y, w * p, 3, fill);
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

static void cover(int b, float x, float y, float w, float h, int kind)
{
    const Book *bk = &books[b];
    const Texture *t = bk->cover[0] ? art_get_file(bk->cover, kind) : NULL;
    if (t) {
        gfx_filter(1);
        gfx_tex(t, x, y, w, h, 0xFFFFFFFF); // the whole cover, never cropped
    } else {
        // A cloth binding with the title stamped on.
        u32 c0, c1;
        ui_placeholder_colors(b * 7 + 3, &c0, &c1);
        gfx_gradient(x, y, w, h, c0, c0, c1, c1);
        gfx_rect(x + 4, y + h * 0.18f, w - 8, 1, BRASS);
        gfx_rect(x + 4, y + h * 0.82f, w - 8, 1, BRASS);
        Font *f = w > 100 ? f_title : f_tiny;
        font_draw_fit(f, x + 6, y + h / 2 + 4, bk->title, PAPER, w - 12);
    }
    if (!bk->finished && bk->last[0]) gfx_rect(x + w - 12, y, 6, h > 100 ? 28 : 16, RED); // bookmark ribbon
}

static void hints_bar(const int *glyph, const char *const *label, int n, float y)
{
    float x = SCREEN_W - 12;
    for (int i = n - 1; i >= 0; i--) {
        if (glyph[i] < 0) {
            x -= font_width(f_tiny, label[i], 0);
            font_draw(f_tiny, x, y, label[i], PAPER2, 0);
        } else {
            x -= ui_hint_width(f_tiny, glyph[i], label[i]);
            ui_hint(f_tiny, x, y, glyph[i], label[i], PAPER2);
        }
        x -= 12;
    }
}

// ---- Shelf --------------------------------------------------------------------------

static void shelf_update(u32 p, u32 nav)
{
    if (!blib_ready()) return;
    if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) { tab ^= 1; sel = scroll = 0; build(); }
    if (p & PSP_CTRL_SQUARE) { sort_title ^= 1; build(); }
    if (nav & PSP_CTRL_LEFT && sel > 0) sel--;
    if (nav & PSP_CTRL_RIGHT && sel < list_n - 1) sel++;
    if (sel < scroll) scroll = sel;
    int current = tab == TAB_SHELF ? blib_current() : -1;
    if (p & PSP_CTRL_TRIANGLE && current >= 0) {
        if (blib_playing() != current) blib_play(current);
        open_now(current);
    }
    if (p & PSP_CTRL_UP && current >= 0) { page = current; button = 0; view = V_CARD; }
    if (p & PSP_CTRL_CROSS && list_n) { page = list[sel]; button = 0; view = V_CARD; }
}

static void now_reading(int b)
{
    float x = 12, y = 38, w = 456, h = 66;
    gfx_rect(x, y, w, h, PAPER);
    gfx_rect(x, y, w, 14, PAPER2);
    font_draw(f_bold, x + 8, y + 11, "NOW READING", RED, 1);
    u32 ms;
    int t = now_track(b, &ms);
    char ch[48], left[48];
    snprintf(ch, sizeof(ch), "Ch %d of %d", t + 1, books[b].track_count);
    font_draw_right(f_bold, x + w - 8, y + 11, ch, INK);
    for (float ly = y + 26; ly < y + h; ly += 12) gfx_rect(x + 98, ly, w - 106, 1, RULE);
    float cw, chh;
    cover_fit(b, ART_COVER_S, 48, 46, &cw, &chh);
    cover(b, x + 8 + (48 - cw) / 2, y + 17 + (46 - chh), cw, chh, ART_COVER_S);
    font_draw_fit(f_title, x + 64, y + 31, books[b].title, INK, 300);
    font_draw_fit(f_text, x + 64, y + 46, books[b].author[0] ? books[b].author : "Unknown author", FADE, 300);
    format_len(left, sizeof(left), books[b].total_ms - blib_elapsed(b));
    strcat(left, " left");
    bar(x + 64, y + 54, 280, progress(b), RED, TRACK_BG);
    font_draw(f_tiny, x + 352, y + 58, left, INK, 0);
}

static void shelf_draw(void)
{
    font_draw(f_head, 14, 28, "Boro Library", PAPER, 0);
    float tx = 160;
    static const char *const tabs[2] = { "Shelf", "Finished" };
    for (int i = 0; i < 2; i++) {
        font_draw(f_text, tx, 26, tabs[i], i == tab ? PAPER : FADE, 0);
        float tw = font_width(f_text, tabs[i], 0);
        if (i == tab) gfx_rect(tx, 30, tw, 2, BRASS);
        tx += tw + 16;
    }
    ui_battery(f_tiny, SCREEN_W - 12, 18, PAPER2);
    if (!blib_ready()) {
        font_draw_center(f_title, SCREEN_W / 2, 140, "Dusting off the shelves\xE2\x80\xA6", PAPER);
        return;
    }
    if (!book_count) {
        gfx_rect(60, 70, 360, 120, PAPER);
        gfx_rect(60, 70, 360, 16, PAPER2);
        font_draw(f_bold, 68, 82, "LIBRARY NOTICE", RED, 1);
        font_draw_center(f_head, SCREEN_W / 2, 118, "No books yet", INK);
        char line[128];
        snprintf(line, sizeof(line), "Copy each book into its own folder in %s/", blib_dir());
        font_draw_center(f_text, SCREEN_W / 2, 142, line, INK);
        font_draw_center(f_small, SCREEN_W / 2, 160, "MP3 or M4B. Files play in name order. Add cover.jpg for the art.", FADE);
        return;
    }
    int current = tab == TAB_SHELF ? blib_current() : -1;
    if (current >= 0) now_reading(current);
    float top = current >= 0 ? 118 : 50;
    if (!list_n) {
        font_draw_center(f_text, SCREEN_W / 2, top + 40, tab == TAB_FINISHED ? "Finished books will rest here." : "Every book is on the go or finished.", PAPER2);
    }
    // Covers stand at their own shapes; scroll so the picked one shows.
    float ax = 0;
    for (;;) {
        float x = 14;
        for (int i = scroll; i < sel; i++) x += cover_aspect(list[i], ART_COVER_S) * 68 + 10;
        if (sel > scroll && x + cover_aspect(list[sel], ART_COVER_S) * 74 > SCREEN_W - 14) scroll++;
        else break;
    }
    for (int i = scroll, x0 = 14; i < list_n && x0 < SCREEN_W; i++) {
        int on = i == sel;
        float h = on ? 74 : 68, w = cover_aspect(list[i], ART_COVER_S) * h, x = x0, y = top + (74 - h) - (on ? 6 : 0);
        if (on) { ui_ring(x - 3, y - 3, w + 6, h + 6, 2, BRASS); ax = x + w / 2; }
        cover(list[i], x, y, w, h, ART_COVER_S);
        if (books[list[i]].finished) {
            gfx_rect(x + w - 26, y + 4, 24, 11, RED);
            font_draw(f_tiny, x + w - 24, y + 13, "READ", PAPER, 0);
        }
        x0 = (int)(x + w + 10);
    }
    gfx_rect(0, top + 74, SCREEN_W, 10, WOOD2);
    gfx_rect(0, top + 74, SCREEN_W, 1, HEX(0x785032, 255));
    gfx_rect(0, top + 84, SCREEN_W, 4, DARKW);
    if (list_n) {
        int b = list[sel];
        ui_triangle(ax - 2, top + 89, UI_UP, BRASS);
        char info[200], len[32];
        format_len(len, sizeof(len), books[b].total_ms);
        if (books[b].finished) snprintf(info, sizeof(info), "%s \xC2\xB7 %s \xC2\xB7 finished", books[b].author[0] ? books[b].author : "Unknown author", len);
        else if (books[b].last[0]) snprintf(info, sizeof(info), "%s \xC2\xB7 %d%% read", books[b].author[0] ? books[b].author : "Unknown author", (int)(progress(b) * 100));
        else snprintf(info, sizeof(info), "%s \xC2\xB7 %s", books[b].author[0] ? books[b].author : "Unknown author", len);
        font_draw_fit(f_title, 14, top + 114, books[b].title, PAPER, 452);
        font_draw_fit(f_text, 14, top + 130, info, FADE, 452);
    }
    static const int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_SQUARE, -1, ICON_CIRCLE };
    const char *l[] = { "Open", "Continue", sort_title ? "Sort: Title" : "Sort: Recent", "L/R Shelf", "Back" };
    if (current >= 0) hints_bar(g, l, 5, 264);
    else {
        static const int g2[] = { ICON_CROSS, ICON_SQUARE, -1, ICON_CIRCLE };
        const char *l2[] = { "Open", l[2], "L/R Shelf", "Back" };
        hints_bar(g2, l2, 4, 264);
    }
}

// ---- Catalog card ---------------------------------------------------------------------

static void card_update(u32 p)
{
    if (p & PSP_CTRL_LEFT && button > 0) button--;
    if (p & PSP_CTRL_RIGHT && button < 2) button++;
    if (p & PSP_CTRL_TRIANGLE) button = 1, p |= PSP_CTRL_CROSS;
    if (p & PSP_CTRL_CIRCLE) { view = V_SHELF; build(); return; }
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
    char crumb[160];
    snprintf(crumb, sizeof(crumb), "Library \xE2\x80\xBA %s", b->title);
    font_draw_fit(f_text, 14, 20, crumb, FADE, 360);
    ui_battery(f_tiny, SCREEN_W - 12, 16, PAPER2);
    float cw, chh;
    cover_fit(page, ART_COVER_L, 130, 196, &cw, &chh);
    cover(page, 14 + (130 - cw) / 2, 36, cw, chh, ART_COVER_L);

    float x = 152, y = 34, w = 314;
    gfx_rect(x, y, w, 200, PAPER);
    gfx_rect(x, y, w, 16, PAPER2);
    font_draw(f_bold, x + 8, y + 12, "CATALOG CARD", RED, 1);
    char no[16];
    snprintf(no, sizeof(no), "No. %04d", page + 1);
    font_draw_right(f_bold, x + w - 8, y + 12, no, INK);
    font_draw_fit(f_head, x + 8, y + 42, b->title, INK, w - 16);
    font_draw_fit(f_text, x + 8, y + 58, b->author[0] ? b->author : "Unknown author", FADE, w - 16);

    char len[32], v[4][120], left[32];
    format_len(len, sizeof(len), b->total_ms);
    snprintf(v[0], sizeof(v[0]), "%s \xC2\xB7 %d %s", len, b->track_count, b->track_count == 1 ? "part" : "chapters");
    format_len(left, sizeof(left), b->total_ms - blib_elapsed(page));
    if (b->finished) snprintf(v[1], sizeof(v[1]), "Finished");
    else if (!b->last[0]) snprintf(v[1], sizeof(v[1]), "Not started");
    else snprintf(v[1], sizeof(v[1]), "%d%% \xC2\xB7 %s left", (int)(progress(page) * 100), left);
    if (b->last[0]) {
        char when[32];
        glib_format_last(when, sizeof(when), b->last);
        snprintf(v[2], sizeof(v[2]), "%s, Ch %d", when, b->pos_track + 1);
    } else {
        snprintf(v[2], sizeof(v[2]), "Never");
    }
    const char *cv = !b->cover[0] ? "no cover" : strstr(b->cover, "/bookart/") ? "art from the MP3" : strrchr(b->cover, '/') + 1;
    const char *fmt = b->tracks[0].kind == BOOK_M4B ? (strcmp(b->tracks[0].path, b->tracks[b->track_count - 1].path) ? "M4B files" : "M4B file")
                      : b->track_count > 1 || strcasecmp(b->dir + strlen(b->dir) - 4, ".mp3") ? "MP3 folder" : "MP3 file";
    snprintf(v[3], sizeof(v[3]), "%s \xC2\xB7 %s", fmt, cv);
    static const char *const keys[4] = { "Length", "Progress", "Last read", "Format" };
    for (int i = 0; i < 4; i++) {
        float ry = y + 70 + i * 20;
        gfx_rect(x + 8, ry + 16, w - 16, 1, RULE);
        font_draw(f_bold, x + 8, ry + 12, keys[i], FADE, 0);
        font_draw_fit(f_text, x + 88, ry + 12, v[i], INK, w - 96);
    }
    const char *labels[3] = { b->finished ? "Listen again" : b->last[0] ? "Continue" : "Start listening", "Contents", "Start over" };
    float bx = x + 8;
    for (int i = 0; i < 3; i++) {
        float bw = font_width(f_bold, labels[i], 0) + 22;
        if (i == button) {
            gfx_rect(bx, y + 164, bw, 22, RED);
        } else {
            gfx_rect(bx, y + 164, bw, 22, PAPER2);
            ui_ring(bx, y + 164, bw, 22, 0, HEX(0xB4A082, 255));
        }
        font_draw(f_bold, bx + 11, y + 179, labels[i], i == button ? PAPER : INK, 0);
        bx += bw + 10;
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
    font_draw(f_text, 14, 20, "Now listening", FADE, 0);
    ui_battery(f_tiny, SCREEN_W - 12, 16, PAPER2);
    // The open book.
    gfx_rect(20, 30, 440, 206, PAPER);
    gfx_gradient(222, 30, 18, 206, HEX(0x785A3C, 0), HEX(0x785A3C, 60), HEX(0x785A3C, 0), HEX(0x785A3C, 60));
    gfx_rect(240, 30, 2, 206, HEX(0xBEAC8C, 255));
    float cw, chh;
    cover_fit(b, ART_COVER_L, 170, 164, &cw, &chh);
    cover(b, 121 - cw / 2, 38 + (164 - chh) / 2, cw, chh, ART_COVER_L);
    font_draw_fit(f_title, 56, 210, bk->title, INK, 170);
    font_draw_fit(f_small, 56, 224, bk->author, FADE, 170);

    u32 ms;
    int t = now_track(b, &ms);
    char rn[24], label[40];
    roman(rn, sizeof(rn), t + 1);
    snprintf(label, sizeof(label), "CHAPTER %s", rn);
    font_draw(f_bold, 258, 50, label, RED, 1);
    font_draw_fit(f_title, 258, 68, bk->names[t], INK, 190);
    u32 len = bk->tracks[t].duration_ms;
    char pos[16], rest[20];
    ui_format_time(pos, sizeof(pos), ms);
    rest[0] = '-';
    ui_format_time(rest + 1, sizeof(rest) - 1, len > ms ? len - ms : 0);
    font_draw(f_tiny, 258, 88, pos, FADE, 0);
    font_draw_right(f_tiny, 448, 88, rest, FADE);
    bar(258, 94, 190, len ? fminf(1.0f, (float)ms / len) : 0, RED, TRACK_BG);

    float cx = 352, cy = 128;
    font_draw_center(f_text, cx - 82, cy + 4, "-30s", FADE);
    font_draw_center(f_text, cx + 82, cy + 4, "+30s", FADE);
    ui_icon(ICON_PREV, cx - 52, cy - 8, 16, INK);
    ui_icon(ICON_NEXT, cx + 36, cy - 8, 16, INK);
    ui_rrect(cx - 15, cy - 15, 30, 30, 15, RED);
    int playing_now = mine && !player_paused();
    ui_icon(playing_now ? ICON_PAUSE : ICON_PLAY, cx - 8, cy - 8, 16, PAPER);

    if (!mine) font_draw_center(f_small, cx, cy + 36, bk->last[0] ? "Paused here \xC2\xB7 press X to listen" : "Press X to start listening", FADE);
    font_draw(f_bold, 258, 190, "Book", FADE, 0);
    float bp = bk->total_ms ? fminf(1.0f, (float)blib_elapsed(b) / bk->total_ms) : 0;
    if (mine && bk->total_ms) {
        u32 done = ms;
        for (int i = 0; i < t; i++) done += bk->tracks[i].duration_ms;
        bp = fminf(1.0f, (float)done / bk->total_ms);
    }
    bar(292, 186, 156, bp, BRASS, TRACK_BG);
    char line[64], left[32];
    format_len(left, sizeof(left), (u32)(bk->total_ms * (1 - bp)));
    snprintf(line, sizeof(line), "%d%% read \xC2\xB7 %s left \xC2\xB7 Ch %d of %d", (int)(bp * 100), left, t + 1, bk->track_count);
    font_draw_fit(f_tiny, 258, 206, line, FADE, 190);

    static const int g[] = { -1, -1, ICON_TRIANGLE, ICON_CROSS, ICON_CIRCLE };
    const char *l[] = { "\xE2\x86\x90\xE2\x86\x92 30s", "L/R Chapter", "Contents", playing_now ? "Pause" : "Play", "Back" };
    hints_bar(g, l, 5, 258);
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
    ui_battery(f_tiny, SCREEN_W - 12, 14, PAPER2);
    gfx_rect(40, 24, 400, 222, PAPER);
    font_draw_center(f_head, SCREEN_W / 2, 48, "Contents", INK);
    font_draw_center(f_small, SCREEN_W / 2, 63, bk->title, FADE);
    u32 ms;
    int cur = now_track(page, &ms);
    for (int i = c_scroll; i < bk->track_count && i < c_scroll + 7; i++) {
        float y = 72 + (i - c_scroll) * 24;
        int on = i == c_sel, done = !bk->finished ? i < cur : 1;
        if (on) {
            gfx_rect(52, y - 1, 376, 22, PAPER2);
            gfx_rect(52, y - 1, 3, 22, RED);
        }
        char rn[24], len[16];
        roman(rn, sizeof(rn), i + 1);
        Font *f = on ? f_bold : f_text;
        u32 col = done && !on ? FADE : INK;
        font_draw(f_bold, 62, y + 14, rn, i == cur ? RED : FADE, 0);
        float nx = 110;
        font_draw_fit(f, nx, y + 14, bk->names[i], col, 250);
        float ex = nx + fminf(250, font_width(f, bk->names[i], 0)) + 6;
        for (float dx = ex; dx < 368; dx += 5) gfx_rect(dx, y + 13, 1, 1, HEX(0xAA9678, 255));
        ui_format_time(len, sizeof(len), bk->tracks[i].duration_ms);
        font_draw_right(f_text, 412, y + 14, len, col);
        if (done && i != cur) {
            gfx_rect(417, y + 9, 2, 2, FADE);
            gfx_rect(419, y + 11, 2, 2, FADE);
            gfx_rect(421, y + 9, 2, 2, FADE);
            gfx_rect(423, y + 7, 2, 2, FADE);
            gfx_rect(425, y + 5, 2, 2, FADE);
        }
    }
    if (c_scroll + 7 < bk->track_count) font_draw_center(f_small, SCREEN_W / 2, 242, "\xE2\x80\xA6", FADE);
    static const int g[] = { ICON_CROSS, ICON_CIRCLE };
    static const char *const l[] = { "Play chapter", "Back" };
    hints_bar(g, l, 2, 264);
}

// ---- Main ------------------------------------------------------------------------------------

int books_update(const Input *in, float dt)
{
    t_now += dt;
    static int was_ready;
    if (blib_ready() && !was_ready) build();
    was_ready = blib_ready();
    u32 p = in->pressed, nav = in->nav;
    if (view != V_SHELF && (page < 0 || page >= book_count)) view = V_SHELF;
    switch (view) {
    case V_SHELF:
        if (p & PSP_CTRL_CIRCLE) return 1;
        shelf_update(p, nav);
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
    gfx_begin(DARKW);
    wood();
    switch (view) {
    case V_SHELF: shelf_draw(); break;
    case V_CARD: card_draw(); break;
    case V_NOW: now_draw(); break;
    case V_CONTENTS: contents_draw(); break;
    }
    ui_volume(f_small, HEX(0x2A1C14, 240), HEX(0x5A4030, 255), BRASS, PAPER);
    gfx_end();
}
