// Head unit theme: a pixel car stereo display. Now playing with a marquee
// title and segmented progress, a radio screen with channel number, tuning
// bar and presets, tabbed browsing, and a color picker on SELECT. Colors come
// from settings; the gradient color cycles its hue.
#include "deck.h"

#include <math.h>
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "library.h"
#include "music.h"
#include "player.h"
#include "playlists.h"
#include "scrub.h"
#include "settings.h"
#include "stations.h"
#include "ui.h"

#define ROW_H 26
#define ROWS 8
#define LIST_Y 36
#define HINT_BASE 265

enum { V_NOW, V_BROWSE };
enum { L_ARTISTS, L_ALBUMS, L_SONGS, L_LISTS, L_STATIONS, TAB_COUNT, L_PICK = TAB_COUNT, L_ARTIST, L_ARTIST_SONGS, L_ALBUM, L_PLAYLIST };
static const char *tab_names[TAB_COUNT] = { "ARTISTS", "ALBUMS", "SONGS", "LISTS", "RADIO" };

typedef struct { int kind, id, sel; float scroll; } List;
static List stack[8];
static int depth = 1, view, tab;
static int color_open, color_sel;
static float t_now, hue, needle = -1;
static char toast[96];
static float toast_t;
static int remove_armed = -1;
static float remove_t;

static u32 FG, BG, DIM, FAINT;

static Font *V(int px) { return ui_font(FAMILY_VT323, 400, px); }
static Font *P(int weight) { return ui_font(FAMILY_SILKSCREEN, weight, 8); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void show_toast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_t = 2.2f;
}

// ---- Colors ------------------------------------------------------------------

static const u32 deck_palette[DECK_GRADIENT][2] = {
    { 0x8FDCFF, 0x04142B }, { 0xFFB547, 0x1E0F00 }, { 0x7DFF8A, 0x031A08 },
    { 0xFF6B6B, 0x200505 }, { 0xE8F0FF, 0x101418 }, { 0xC9A2FF, 0x120820 },
};

static u32 hsl(float h, float s, float l)
{
    float c = (1 - fabsf(2 * l - 1)) * s, hp = fmodf(h, 360) / 60, x = c * (1 - fabsf(fmodf(hp, 2) - 1)), m = l - c / 2;
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    return RGBA((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255), 255);
}

static void colors_for(int c, float h, u32 *fg, u32 *bg)
{
    if (c == DECK_GRADIENT) {
        *fg = hsl(h, 0.95f, 0.72f);
        *bg = hsl(h, 0.8f, 0.07f);
    } else {
        *fg = HEX(deck_palette[c][0], 255);
        *bg = HEX(deck_palette[c][1], 255);
    }
}

static void palette(void)
{
    colors_for(settings.deck_color, hue, &FG, &BG);
    DIM = color_lerp(BG, FG, 0.5f);
    FAINT = color_lerp(BG, FG, 0.17f);
}

// ---- Text --------------------------------------------------------------------

// Uppercase ASCII copy; a few rotate so one call can feed another.
static const char *up(const char *s)
{
    static char buf[4][192];
    static int n;
    char *o = buf[n++ & 3];
    int i = 0;
    for (; s[i] && i < 191; i++) o[i] = s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i];
    o[i] = 0;
    return o;
}

// Text that scrolls in 2px steps when it doesn't fit.
static void marquee(Font *f, float x, float base, float w, const char *s, u32 color)
{
    float tw = font_width(f, s, 0), a = font_ascent(f);
    if (tw <= w) { font_draw(f, x, base, s, color, 0); return; }
    float gap = 48, off = floorf(fmodf(t_now * 36, tw + gap) / 2) * 2;
    gfx_clip(x, base - a - 4, w, a * 1.6f + 4);
    font_draw(f, x - off, base, s, color, 0);
    font_draw(f, x - off + tw + gap, base, s, color, 0);
    gfx_unclip();
}

static void tri_right(float x, float cy, u32 color)
{
    for (int k = 0; k < 5; k++) gfx_rect(x + k * 2, cy - 8 + k * 2, 2, 16 - k * 4, color);
}

// ---- Stations ----------------------------------------------------------------

static int current_station(void)
{
    if (!player_locked()) return -1;
    int kind;
    const char *src = player_source(&kind);
    for (int i = 0; i < station_count; i++) {
        const char *name = station_name(&stations[i]);
        size_t n = strlen(name);
        if (!strncmp(src, name, n) && !strcmp(src + n, " Radio")) return i;
    }
    return -1;
}

static void play_station(int index)
{
    if (!station_play(index)) { show_toast("NO SONGS ON THIS STATION"); return; }
    view = V_NOW;
}

// ---- Lists -------------------------------------------------------------------

static List *top(void) { return &stack[depth - 1]; }

static void push(int kind, int id)
{
    if (depth < 8) stack[depth++] = (List){ kind, id, 0, 0 };
}

static int list_count(const List *l)
{
    switch (l->kind) {
    case L_ARTISTS: return lib.artist_count;
    case L_ALBUMS: return lib.album_count;
    case L_SONGS: return lib.songs_az_count;
    case L_LISTS: return playlist_count;
    case L_STATIONS: return station_count + 1;
    case L_PICK: return lib.radio_count + lib.artist_count;
    case L_ARTIST: return 1 + lib.artists[l->id].album_count;
    case L_ARTIST_SONGS: return lib.artists[l->id].song_count;
    case L_ALBUM: return lib.albums[l->id].count;
    case L_PLAYLIST: return playlists[l->id].count;
    }
    return 0;
}

static const int *list_songs(const List *l, int *count, int *from, const char **name)
{
    switch (l->kind) {
    case L_SONGS: *count = lib.songs_az_count; *from = FROM_SONGS; *name = "Songs"; return lib.songs_az;
    case L_ARTIST_SONGS: {
        const Artist *ar = &lib.artists[l->id];
        *count = ar->song_count; *from = FROM_ARTIST; *name = ar->name;
        return ar->songs;
    }
    case L_ALBUM: {
        const Album *a = &lib.albums[l->id];
        *count = a->count; *from = FROM_ALBUM; *name = a->name;
        return a->songs;
    }
    case L_PLAYLIST: {
        const Playlist *pl = &playlists[l->id];
        *count = pl->count; *from = FROM_PLAYLIST; *name = pl->name;
        return pl->songs;
    }
    }
    return NULL;
}

static const char *list_title(const List *l)
{
    switch (l->kind) {
    case L_PICK: return "NEW STATION";
    case L_ARTIST: case L_ARTIST_SONGS: return up(lib.artists[l->id].name);
    case L_ALBUM: return up(lib.albums[l->id].name);
    case L_PLAYLIST: return up(playlists[l->id].name);
    }
    return tab_names[l->kind];
}

// Row text, plus a short note shown on the right.
static const char *row_label(const List *l, int i, char *note, int size)
{
    int n, from;
    const char *name;
    note[0] = 0;
    const int *songs = list_songs(l, &n, &from, &name);
    if (songs) {
        const Song *s = &lib.songs[songs[i]];
        ui_format_time(note, size, s->duration_ms);
        return up(song_title(s));
    }
    switch (l->kind) {
    case L_ARTISTS: snprintf(note, size, "%d SONGS", lib.artists[i].song_count); return up(lib.artists[i].name);
    case L_ALBUMS: {
        const Album *a = &lib.albums[library_albums_sorted(0)[i]];
        snprintf(note, size, "%s", up(a->artist));
        return up(a->name);
    }
    case L_LISTS: snprintf(note, size, "%d SONGS", playlists[i].count); return up(playlists[i].name);
    case L_STATIONS: {
        if (i == 0) return "+ NEW STATION";
        static char row[160];
        snprintf(row, sizeof(row), "CH%02d  %s", i, up(station_name(&stations[i - 1])));
        if (stations[i - 1].radio) snprintf(note, size, "RADIO ONLY");
        return row;
    }
    case L_PICK:
        if (i < lib.radio_count) { snprintf(note, size, "RADIO ONLY"); return up(lib.radio[i].name); }
        return up(lib.artists[i - lib.radio_count].name);
    case L_ARTIST:
        if (i == 0) { snprintf(note, size, "%d SONGS", lib.artists[l->id].song_count); return "ALL SONGS"; }
        return up(lib.albums[lib.artists[l->id].albums[i - 1]].name);
    }
    return "";
}

// ---- Update ------------------------------------------------------------------

void deck_enter(void)
{
    depth = 1;
    stack[0] = (List){ tab, 0, 0, 0 };
    view = player_song() >= 0 ? V_NOW : V_BROWSE;
    color_open = 0;
    needle = -1;
}

static void play_list(const List *l, int start, int shuffle)
{
    int n, from;
    const char *name;
    const int *songs = list_songs(l, &n, &from, &name);
    if (!songs || !n) return;
    player_play_from(from, name, songs, n, start, shuffle);
    view = V_NOW;
}

static void select_row(List *l)
{
    int i = l->sel;
    switch (l->kind) {
    case L_ARTISTS: push(L_ARTIST, i); break;
    case L_ALBUMS: push(L_ALBUM, library_albums_sorted(0)[i]); break;
    case L_LISTS: push(L_PLAYLIST, i); break;
    case L_ARTIST:
        if (i == 0) push(L_ARTIST_SONGS, l->id);
        else push(L_ALBUM, lib.artists[l->id].albums[i - 1]);
        break;
    case L_STATIONS:
        if (i == 0) push(L_PICK, 0);
        else play_station(i - 1);
        break;
    case L_PICK: {
        int radio = i < lib.radio_count;
        int s = stations_add(radio, radio ? i : i - lib.radio_count);
        depth--;
        top()->sel = s + 1;
        play_station(s);
        break;
    }
    default: play_list(l, i, 0); break;
    }
}

static void remove_station(List *l)
{
    int i = l->sel - 1;
    if (i < 0) return;
    if (stations[i].radio) { show_toast("RADIO-ONLY STATIONS STAY"); return; }
    if (remove_armed == i && remove_t > 0) {
        stations_remove(i);
        remove_armed = -1;
        l->sel = clampi(l->sel, 0, station_count);
        show_toast("STATION REMOVED");
        return;
    }
    remove_armed = i;
    remove_t = 2.2f;
    show_toast("START AGAIN TO REMOVE");
}

static int update_browse(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    List *l = top();
    if (depth == 1 && (n & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER))) {
        tab = (tab + ((n & PSP_CTRL_RTRIGGER) ? 1 : TAB_COUNT - 1)) % TAB_COUNT;
        stack[0] = (List){ tab, 0, 0, 0 };
        return MUSIC_STAY;
    }
    int count = list_count(l);
    l->sel = clampi(l->sel, 0, count ? count - 1 : 0);
    if (count) {
        if (n & PSP_CTRL_UP) l->sel = clampi(l->sel - 1, 0, count - 1);
        if (n & PSP_CTRL_DOWN) l->sel = clampi(l->sel + 1, 0, count - 1);
        if (depth > 1 && (n & PSP_CTRL_LTRIGGER)) l->sel = clampi(l->sel - ROWS, 0, count - 1);
        if (depth > 1 && (n & PSP_CTRL_RTRIGGER)) l->sel = clampi(l->sel + ROWS, 0, count - 1);
        if (p & PSP_CTRL_CROSS) select_row(l);
        else if (p & PSP_CTRL_START) {
            if (l->kind == L_STATIONS) remove_station(l);
            else if (l->kind == L_ARTIST) {
                const Artist *ar = &lib.artists[l->id];
                player_play_from(FROM_ARTIST, ar->name, ar->songs, ar->song_count, -1, 1);
                view = V_NOW;
            } else play_list(l, -1, 1);
        }
    }
    if (p & PSP_CTRL_TRIANGLE) {
        if (player_song() >= 0) view = V_NOW;
        else show_toast("NO DISC");
    }
    if (p & PSP_CTRL_CIRCLE) {
        if (depth == 1) return MUSIC_HOME;
        depth--;
    }
    return MUSIC_STAY;
}

static void update_now(const Input *in, float dt)
{
    u32 p = in->pressed, n = in->nav;
    if (player_song() < 0) { view = V_BROWSE; return; }
    if (p & PSP_CTRL_CROSS) player_toggle_pause();
    if (player_locked()) {
        // L/R and left/right tune to the next station.
        int dir = (n & (PSP_CTRL_RTRIGGER | PSP_CTRL_RIGHT)) ? 1 : (n & (PSP_CTRL_LTRIGGER | PSP_CTRL_LEFT)) ? -1 : 0;
        int cur = current_station();
        if (dir && station_count) play_station(((cur < 0 ? 0 : cur) + dir + station_count) % station_count);
    } else {
        scrub_update(in, dt);
        if (p & PSP_CTRL_RTRIGGER) player_next();
        if (p & PSP_CTRL_LTRIGGER) player_prev();
        if (p & PSP_CTRL_SQUARE) {
            player_set_shuffle(!player_shuffle());
            show_toast(player_shuffle() ? "RANDOM ON" : "RANDOM OFF");
        }
    }
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) view = V_BROWSE;
}

static void update_color(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    if (color_sel < DECK_COLORS) {
        if ((n & PSP_CTRL_LEFT) && color_sel % 4) color_sel--;
        if ((n & PSP_CTRL_RIGHT) && color_sel % 4 < 3 && color_sel + 1 < DECK_COLORS) color_sel++;
        if ((n & PSP_CTRL_UP) && color_sel >= 4) color_sel -= 4;
        if (n & PSP_CTRL_DOWN) color_sel = color_sel + 4 < DECK_COLORS ? color_sel + 4 : DECK_COLORS;
        if (color_sel < DECK_COLORS) settings.deck_color = color_sel;
    } else {
        if (n & PSP_CTRL_LEFT) settings.deck_speed = clampi(settings.deck_speed - 1, 0, 2);
        if (n & PSP_CTRL_RIGHT) settings.deck_speed = clampi(settings.deck_speed + 1, 0, 2);
        if (n & PSP_CTRL_UP) color_sel = DECK_GRADIENT;
    }
    if (p & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE | PSP_CTRL_SELECT)) {
        color_open = 0;
        settings_save();
    }
}

int deck_update(const Input *in, float dt)
{
    static const float speeds[3] = { 5, 12, 30 };
    t_now += dt;
    hue = fmodf(hue + speeds[settings.deck_speed] * dt, 360);
    if (toast_t > 0) toast_t -= dt;
    if (remove_t > 0) remove_t -= dt;
    u32 p = in->pressed;
    if (library_state() != LIB_READY) return (p & PSP_CTRL_CIRCLE) ? MUSIC_HOME : MUSIC_STAY;
    if (color_open) { update_color(in); return MUSIC_STAY; }
    if (p & PSP_CTRL_SELECT) {
        color_open = 1;
        color_sel = settings.deck_color;
        return MUSIC_STAY;
    }
    if (view == V_NOW) { update_now(in, dt); return MUSIC_STAY; }
    if ((p & PSP_CTRL_SQUARE) && player_song() >= 0) player_toggle_pause();
    return update_browse(in);
}

// ---- Drawing -----------------------------------------------------------------

static void draw_scanlines(void)
{
    u32 h = RGBA(0, 0, 0, 77), v = RGBA(0, 0, 0, 56);
    for (int y = 2; y < SCREEN_H; y += 3) gfx_rect(0, y, SCREEN_W, 1, h);
    for (int x = 2; x < SCREEN_W; x += 3) gfx_rect(x, 0, 1, SCREEN_H, v);
}

static void draw_battery(float right, float cy)
{
    int charging, pct = sys_battery(&charging);
    float x = right - 20;
    gfx_rect(x, cy - 4, 18, 1, FG);
    gfx_rect(x, cy + 4, 18, 1, FG);
    gfx_rect(x, cy - 4, 1, 9, FG);
    gfx_rect(x + 17, cy - 4, 1, 9, FG);
    gfx_rect(x + 18, cy - 2, 2, 5, FG);
    int bars = pct < 0 ? 4 : (pct + 24) / 25;
    for (int i = 0; i < 4; i++) gfx_rect(x + 2 + i * 4, cy - 2, 3, 5, i < bars ? FG : FAINT);
    if (pct >= 0) {
        char s[16];
        snprintf(s, sizeof(s), "%d%%", pct);
        font_draw_right(P(400), x - 5, cy + 3, s, DIM);
    }
}

// FL/AC badge, indicators, clock and battery between two rules.
static void draw_header(const char *mode, const char *track)
{
    gfx_rect(0, 2, SCREEN_W, 1, DIM);
    gfx_rect(0, 29, SCREEN_W, 1, DIM);
    Font *b = ui_font(FAMILY_SILKSCREEN, 700, 16), *s = P(400);
    float w = font_width(b, "FL/AC", 0) + 10;
    gfx_rect(10, 7, w, 18, FG);
    font_draw(b, 15, 22, "FL/AC", BG, 0);
    float x = 10 + w + 10;
    if (mode) {
        float mw = font_width(s, mode, 0) + 8;
        gfx_rect(x, 11, mw, 1, DIM);
        gfx_rect(x, 21, mw, 1, DIM);
        gfx_rect(x, 11, 1, 11, DIM);
        gfx_rect(x + mw - 1, 11, 1, 11, DIM);
        font_draw(s, x + 4, 19, mode, FG, 0);
        x += mw + 10;
    }
    if (track) {
        x = font_draw(s, x, 19, track, FG, 0) + 12;
    }
    if (mode && !player_locked()) {
        x = font_draw(s, x, 19, "RDM", player_shuffle() ? FG : FAINT, 0) + 8;
        font_draw(s, x, 19, "RPT", FAINT, 0);
    }
    draw_battery(SCREEN_W - 10, 16);
    char clock[16];
    sys_clock(clock, sizeof(clock));
    font_draw_right(s, SCREEN_W - 70, 19, up(clock), FG);
}

static void draw_hints(const int *glyphs, const char **labels, int n)
{
    Font *f = P(400);
    float x = SCREEN_W - 10;
    for (int i = n - 1; i >= 0; i--) {
        x -= glyphs[i] >= 0 ? ui_hint_width(f, glyphs[i], labels[i]) : font_width(f, labels[i], 0);
        if (glyphs[i] >= 0) ui_hint(f, x, HINT_BASE, glyphs[i], labels[i], DIM);
        else font_draw(f, x, HINT_BASE, labels[i], DIM, 0);
        x -= 12;
    }
}

// Spectrum-style bars built from segments; they settle while paused.
static void draw_eq(float x, float y, float w, float h, int bars)
{
    float bw = w / bars;
    int segs = (int)(h / 4);
    for (int i = 0; i < bars; i++) {
        float k = player_paused() || player_song() < 0 ? 0.08f
                : 0.5f + 0.25f * sinf(t_now * (3.1f + i * 0.37f) + i) + 0.2f * sinf(t_now * (7.3f - i * 0.21f) + i * 2.3f);
        k *= 1 - 0.35f * fabsf((float)i / bars - 0.4f);
        int lit = (int)(k * segs + 0.5f);
        for (int sgm = 0; sgm < segs; sgm++)
            gfx_rect(x + i * bw, y + h - (sgm + 1) * 4 + 1, bw - 2, 3, sgm < lit ? (sgm >= segs - 2 ? FG : DIM) : FAINT);
    }
}

static void draw_now(void)
{
    int song = player_song();
    const Song *s = &lib.songs[song];
    int total, idx = player_index(&total);
    char trk[24];
    snprintf(trk, sizeof(trk), "TRK %02d/%02d", idx + 1, total);
    draw_header("MP3", trk);

    marquee(V(46), 16, 88, SCREEN_W - 32, up(song_title(s)), FG);
    char line[256];
    snprintf(line, sizeof(line), "%s \xC2\xB7 %s", up(song_artist(s)), up(song_album(s)));
    font_draw_fit(V(22), 16, 116, line, DIM, SCREEN_W - 32);

    u32 pos = scrub_position(), dur = s->duration_ms;
    char a[16], b[16];
    ui_format_time(a, sizeof(a), pos);
    ui_format_time(b, sizeof(b), dur);
    snprintf(line, sizeof(line), "%s / %s", a, b);
    font_draw(V(22), 16, 142, line, FG, 0);
    font_draw_right(V(22), SCREEN_W - 16, 142, player_paused() ? "PAUSE" : "PLAY", player_paused() ? DIM : FG);

    float prog = dur ? (float)pos / dur : 0;
    int lit = (int)(prog * 40 + 0.5f);
    for (int i = 0; i < 40; i++) gfx_rect(21 + i * 11, 152, 9, 10, i < lit ? FG : FAINT);

    draw_eq(16, 176, SCREEN_W - 32, 72, 32);
    const int g[] = { ICON_CROSS, -1, ICON_SQUARE, ICON_TRIANGLE, -1 };
    const char *l[] = { player_paused() ? "PLAY" : "PAUSE", "L/R TRACK", "RANDOM", "BROWSE", "SELECT COLOR" };
    draw_hints(g, l, 5);
}

static void draw_radio(void)
{
    int song = player_song(), cur = current_station();
    const Song *s = &lib.songs[song];
    draw_header("FM", "RADIO");
    char ch[16];
    snprintf(ch, sizeof(ch), "CH %02d", cur + 1);
    font_draw(V(64), 14, 92, ch, FG, 0);
    float x = 160, w = SCREEN_W - 16 - x;
    int kind;
    marquee(V(32), x, 60, w, up(player_source(&kind)), FG);
    font_draw_fit(V(22), x, 82, up(song_title(s)), FG, w);
    font_draw_fit(V(22), x, 100, up(song_artist(s)), DIM, w);

    // Tuning scale with a needle that sweeps to the channel.
    float sx = 16, sw = SCREEN_W - 32, sy = 116;
    for (int i = 0; i <= 80; i++) {
        float tx = sx + sw * i / 80;
        gfx_rect(tx, sy + (i % 10 ? 8 : 2), 1, i % 10 ? 6 : 12, i % 10 ? FAINT : DIM);
    }
    Font *f = P(400);
    for (int i = 0; i <= 4; i++) {
        char mhz[8];
        snprintf(mhz, sizeof(mhz), "%d", 88 + i * 5);
        font_draw_center(f, sx + sw * i / 4, sy + 24, mhz, DIM);
    }
    int slots = station_count > 6 ? station_count : 6;
    float target = sx + sw * ((cur < 0 ? 0 : cur) + 0.5f) / slots;
    if (needle < 0) needle = target;
    gfx_rect(floorf(needle), sy - 2, 2, 18, FG);

    draw_eq(16, 148, SCREEN_W - 32, 48, 32);

    // Presets: the first six stations.
    float pw = (SCREEN_W - 32 - 5 * 6) / 6.0f;
    for (int i = 0; i < 6; i++) {
        float px = 16 + i * (pw + 6), py = 206;
        int on = i == cur, has = i < station_count;
        if (on) gfx_rect(px, py, pw, 34, FG);
        else {
            gfx_rect(px, py, pw, 1, FAINT);
            gfx_rect(px, py + 33, pw, 1, FAINT);
            gfx_rect(px, py, 1, 34, FAINT);
            gfx_rect(px + pw - 1, py, 1, 34, FAINT);
        }
        char pn[4];
        snprintf(pn, sizeof(pn), "P%d", i + 1);
        font_draw(f, px + 5, py + 12, pn, on ? BG : DIM, 0);
        if (has) font_draw_fit(V(18), px + 5, py + 29, up(station_name(&stations[i])), on ? BG : FG, pw - 8);
    }
    const int g[] = { ICON_CROSS, -1, ICON_TRIANGLE, -1 };
    const char *l[] = { player_paused() ? "PLAY" : "PAUSE", "L/R STATION", "BROWSE", "SELECT COLOR" };
    draw_hints(g, l, 4);
}

static void draw_browse(float dt)
{
    List *l = top();
    draw_header(NULL, NULL);
    Font *s = P(400);
    if (depth == 1) {
        // Tabs between the badge and the clock.
        float x = 92;
        for (int i = 0; i < TAB_COUNT; i++) {
            float w = font_width(s, tab_names[i], 0) + 10;
            if (i == tab) gfx_rect(x, 9, w, 14, FG);
            font_draw(s, x + 5, 19, tab_names[i], i == tab ? BG : DIM, 0);
            x += w + 2;
        }
    } else {
        font_draw_fit(s, 92, 19, list_title(l), FG, 220);
    }

    int count = list_count(l);
    if (!count) {
        const char *msg = l->kind == L_LISTS ? "MAKE PLAYLISTS IN FL/AC" : "EMPTY";
        font_draw(V(27), 16, LIST_Y + 24, msg, DIM, 0);
    }
    float view_h = ROWS * ROW_H, target = l->scroll, rowy = l->sel * ROW_H, max = count * ROW_H - view_h;
    if (rowy < target) target = rowy;
    if (rowy + ROW_H > target + view_h) target = rowy + ROW_H - view_h;
    if (target > max) target = max;
    if (target < 0) target = 0;
    l->scroll = floorf(ui_approach(l->scroll, target, dt, 22));

    Font *f = V(27);
    float w = SCREEN_W - 20;
    gfx_clip(0, LIST_Y, SCREEN_W, view_h);
    int first = (int)(l->scroll / ROW_H);
    for (int i = first; i < count && i <= first + ROWS; i++) {
        float y = LIST_Y + i * ROW_H - l->scroll;
        int sel = i == l->sel;
        char note[64];
        const char *label = row_label(l, i, note, sizeof(note));
        if (sel) {
            gfx_rect(8, y + 1, w, ROW_H - 2, FG);
            if (l->kind == L_STATIONS) {
                // The tuning sweep across the selected station.
                float sx = 8 + fmodf(t_now * 160, w + 60) - 60;
                gfx_rect(fmaxf(8, sx), y + 1, fminf(40, 8 + w - fmaxf(8, sx)), ROW_H - 2, color_lerp(FG, HEX(0xFFFFFF, 255), 0.35f));
            }
            tri_right(14, y + ROW_H / 2, BG);
        }
        float nw = note[0] ? font_width(s, note, 0) : 0;
        font_draw_fit(f, 30, y + 20, label, sel ? BG : FG, w - 30 - (nw ? nw + 14 : 4));
        if (nw) font_draw(s, 8 + w - 6 - nw, y + 17, note, sel ? BG : DIM, 0);
    }
    gfx_unclip();
    if (count > ROWS) {
        int segs = 26, on = (int)(segs * (max > 0 ? l->scroll / max : 0) + 0.5f);
        for (int i = 0; i < segs; i++) gfx_rect(SCREEN_W - 8, LIST_Y + 2 + i * 8, 3, 6, abs(i - on) <= 1 ? FG : FAINT);
    }

    int n, from;
    const char *name;
    if (list_songs(l, &n, &from, &name) || l->kind == L_ARTIST) {
        const int g[] = { ICON_CROSS, -1, ICON_CIRCLE, -1 };
        const char *lb[] = { "PLAY", "START RANDOM", "BACK", "SELECT COLOR" };
        draw_hints(g, lb, 4);
    } else if (l->kind == L_STATIONS && l->sel > 0 && !stations[l->sel - 1].radio) {
        const int g[] = { ICON_CROSS, -1, ICON_CIRCLE, -1 };
        const char *lb[] = { "TUNE", "START REMOVE", "BACK", "SELECT COLOR" };
        draw_hints(g, lb, 4);
    } else {
        const int g[] = { ICON_CROSS, -1, ICON_TRIANGLE, ICON_CIRCLE, -1 };
        const char *lb[] = { "OPEN", "L/R TAB", "PLAYING", "HOME", "SELECT COLOR" };
        if (depth == 1) draw_hints(g, lb, 5);
        else draw_hints((const int[]){ ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE, -1 },
                        (const char *[]){ "OPEN", "PLAYING", "BACK", "SELECT COLOR" }, 4);
    }
}

static void draw_color(void)
{
    draw_header(NULL, NULL);
    Font *s = P(400);
    font_draw(s, 92, 19, "DISPLAY COLOR", FG, 0);
    float cw = 106, ch = 52;
    for (int i = 0; i < DECK_COLORS; i++) {
        float x = 16 + (i % 4) * (cw + 6), y = 40 + (i / 4) * (ch + 8);
        u32 fg, bg;
        colors_for(i, hue, &fg, &bg);
        gfx_rect(x, y, cw, ch, bg);
        if (i == DECK_GRADIENT) {
            for (int k = 0; k < 6; k++) {
                u32 c0 = hsl(k * 60, 0.95f, 0.72f), c1 = hsl(k * 60 + 60, 0.95f, 0.72f);
                gfx_gradient(x + 8 + k * (cw - 16) / 6, y + 34, (cw - 16) / 6 + 1, 8, c0, c1, c0, c1);
            }
        } else {
            for (int k = 0; k < 8; k++) gfx_rect(x + 8 + k * 11, y + 34, 9, 8, k < 6 ? fg : color_lerp(bg, fg, 0.17f));
        }
        font_draw(V(22), x + 8, y + 24, up(deck_color_names[i]), fg, 0);
        int sel = i == color_sel, cur = i == settings.deck_color;
        u32 edge = sel ? FG : cur ? DIM : FAINT;
        float t = sel ? 2 : 1;
        gfx_rect(x - t, y - t, cw + 2 * t, t, edge);
        gfx_rect(x - t, y + ch, cw + 2 * t, t, edge);
        gfx_rect(x - t, y, t, ch, edge);
        gfx_rect(x + cw, y, t, ch, edge);
    }
    // Gradient speed.
    static const char *speeds[3] = { "SLOW", "MED", "FAST" };
    float y = 170;
    int focus = color_sel == DECK_COLORS;
    font_draw(s, 16, y + 11, "GRADIENT SPEED", focus ? FG : DIM, 0);
    float x = 130;
    for (int i = 0; i < 3; i++) {
        float w = font_width(s, speeds[i], 0) + 16;
        int on = i == settings.deck_speed;
        if (on) gfx_rect(x, y, w, 16, focus ? FG : DIM);
        font_draw(s, x + 8, y + 11, speeds[i], on ? BG : DIM, 0);
        x += w + 4;
    }
    // Preview.
    gfx_rect(16, 198, SCREEN_W - 32, 1, FAINT);
    gfx_rect(16, 240, SCREEN_W - 32, 1, FAINT);
    int song = player_song();
    marquee(V(32), 24, 230, SCREEN_W - 48, song >= 0 ? up(song_title(&lib.songs[song])) : "BORO HEAD UNIT", FG);
    const int g[] = { -1, ICON_CROSS };
    const char *lb[] = { "ARROWS CHOOSE", "DONE" };
    draw_hints(g, lb, 2);
}

static void draw_toast(void)
{
    if (toast_t <= 0) return;
    Font *f = V(22);
    float w = font_width(f, toast, 0) + 20, x = (SCREEN_W - w) / 2, y = 112;
    gfx_rect(x - 2, y - 2, w + 4, 32, FG);
    gfx_rect(x, y, w, 28, BG);
    font_draw(f, x + 10, y + 20, toast, FG, 0);
}

void deck_draw(float dt)
{
    palette();
    gfx_begin(BG);
    // A faint glow in the middle of the display.
    gfx_gradient(0, 0, SCREEN_W, SCREEN_H / 2, BG, BG, color_lerp(BG, FG, 0.06f), color_lerp(BG, FG, 0.06f));
    gfx_gradient(0, SCREEN_H / 2, SCREEN_W, SCREEN_H / 2, color_lerp(BG, FG, 0.06f), color_lerp(BG, FG, 0.06f), BG, BG);
    if (library_state() != LIB_READY) {
        draw_header(NULL, NULL);
        char msg[64];
        snprintf(msg, sizeof(msg), "READING DISC %d", library_scan_count());
        font_draw_center(V(32), SCREEN_W / 2, SCREEN_H / 2, msg, FG);
    } else if (color_open) {
        draw_color();
    } else if (view == V_NOW && player_song() >= 0) {
        if (player_locked()) {
            int cur = current_station();
            int slots = station_count > 6 ? station_count : 6;
            float target = 16 + (SCREEN_W - 32) * ((cur < 0 ? 0 : cur) + 0.5f) / slots;
            needle = needle < 0 ? target : ui_approach(needle, target, dt, 6);
            draw_radio();
        } else draw_now();
    } else {
        draw_browse(dt);
    }
    draw_toast();
    draw_scanlines();
    ui_volume(V(22), BG, FAINT, FG, FG);
    gfx_end();
}
