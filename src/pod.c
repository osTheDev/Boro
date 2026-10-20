// iPod theme: a classic white menu on the left half with art for the
// selected row on the right, drill-down lists for albums, artists, songs,
// playlists and radio stations, and now playing with a reflected cover.
#include "pod.h"

#include <math.h>
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>
#include "art.h"
#include "library.h"
#include "music.h"
#include "player.h"
#include "playlists.h"
#include "scrub.h"
#include "stations.h"
#include "ui.h"

#define BG HEX(0xFFFFFF, 255)
#define TEXT HEX(0x111111, 255)
#define MUTED HEX(0x6B7078, 255)
#define HL HEX(0x1A73E8, 255)
#define HL_TOP HEX(0x4A93F0, 255)
#define BAR_TOP HEX(0xF7F7F7, 255)
#define BAR_BOT HEX(0xD4D6DA, 255)
#define BORDER HEX(0x9EA2A8, 255)
#define HINT_BG HEX(0xECEDEF, 255)
#define DIVIDER HEX(0xC9CCD1, 255)
#define WHITE HEX(0xFFFFFF, 255)

#define TITLE_H 22
#define ROW_H 29
#define ROWS 8
#define LIST_W 240
#define HINT_Y 256

enum { L_MENU, L_ALBUMS, L_ARTISTS, L_SONGS, L_PLAYLISTS, L_RADIO, L_PICK, L_ARTIST, L_ARTIST_SONGS, L_ALBUM, L_PLAYLIST };
enum { M_ALBUMS, M_ARTISTS, M_SONGS, M_PLAYLISTS, M_RADIO, M_SETTINGS, M_NOW, M_COUNT };
static const char *menu_names[M_COUNT] = { "Albums", "Artists", "Songs", "Playlists", "Radio", "Settings", "Now Playing" };

typedef struct { int kind, id, sel; float scroll; } List;
static List stack[8];
static int depth = 1, now_open;
static float t_now;
static char toast[96];
static float toast_t;
static int remove_armed = -1;
static float remove_t;

static Font *A(int weight, int px) { return ui_font(FAMILY_ARCHIVO, weight, px); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void show_toast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_t = 2.2f;
}

static List *top(void) { return &stack[depth - 1]; }

static void push(int kind, int id)
{
    if (depth < 8) stack[depth++] = (List){ kind, id, 0, 0 };
}

static void open_now(void)
{
    if (player_song() >= 0) now_open = 1;
    else show_toast("Nothing is playing");
}

void pod_enter(void)
{
    depth = 1;
    stack[0] = (List){ L_MENU, 0, 0, 0 };
    now_open = 0;
}

// ---- Lists -------------------------------------------------------------------

static int menu_count(void) { return player_song() >= 0 ? M_COUNT : M_COUNT - 1; }

static int list_count(const List *l)
{
    switch (l->kind) {
    case L_MENU: return menu_count();
    case L_ALBUMS: return lib.album_count;
    case L_ARTISTS: return lib.artist_count;
    case L_SONGS: return lib.songs_az_count;
    case L_PLAYLISTS: return playlist_count;
    case L_RADIO: return station_count + 1;
    case L_PICK: return lib.radio_count + lib.artist_count;
    case L_ARTIST: return 1 + lib.artists[l->id].album_count;
    case L_ARTIST_SONGS: return lib.artists[l->id].song_count;
    case L_ALBUM: return lib.albums[l->id].count;
    case L_PLAYLIST: return playlists[l->id].count;
    }
    return 0;
}

// The songs a song list plays, or NULL for lists of other things.
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
    case L_ALBUMS: return "Albums";
    case L_ARTISTS: return "Artists";
    case L_SONGS: return "Songs";
    case L_PLAYLISTS: return "Playlists";
    case L_RADIO: return "Radio";
    case L_PICK: return "New Station";
    case L_ARTIST: case L_ARTIST_SONGS: return lib.artists[l->id].name;
    case L_ALBUM: return lib.albums[l->id].name;
    case L_PLAYLIST: return playlists[l->id].name;
    }
    return "";
}

static const char *row_label(const List *l, int i)
{
    int n, from;
    const char *name;
    const int *songs = list_songs(l, &n, &from, &name);
    if (songs) return song_title(&lib.songs[songs[i]]);
    switch (l->kind) {
    case L_MENU: return menu_names[i];
    case L_ALBUMS: return lib.albums[library_albums_sorted(0)[i]].name;
    case L_ARTISTS: return lib.artists[i].name;
    case L_PLAYLISTS: return playlists[i].name;
    case L_RADIO: return i ? station_name(&stations[i - 1]) : "New Station\xE2\x80\xA6";
    case L_PICK: return i < lib.radio_count ? lib.radio[i].name : lib.artists[i - lib.radio_count].name;
    case L_ARTIST: return i ? lib.albums[lib.artists[l->id].albums[i - 1]].name : "All Songs";
    }
    return "";
}

static int row_opens_list(const List *l, int i)
{
    switch (l->kind) {
    case L_MENU: return i != M_NOW && i != M_SETTINGS;
    case L_ALBUMS: case L_ARTISTS: case L_PLAYLISTS: case L_ARTIST: return 1;
    case L_RADIO: return i == 0;
    }
    return 0;
}

// ---- Update ------------------------------------------------------------------

static void play_list(const List *l, int start, int shuffle)
{
    int n, from;
    const char *name;
    const int *songs = list_songs(l, &n, &from, &name);
    if (!songs || !n) return;
    player_play_from(from, name, songs, n, start, shuffle);
    now_open = 1;
}

static void play_station(int index)
{
    if (!station_play(index)) { show_toast("This station has no songs"); return; }
    now_open = 1;
}

static int select_row(List *l)
{
    int i = l->sel;
    switch (l->kind) {
    case L_MENU:
        switch (i) {
        case M_ALBUMS: push(L_ALBUMS, 0); break;
        case M_ARTISTS: push(L_ARTISTS, 0); break;
        case M_SONGS: push(L_SONGS, 0); break;
        case M_PLAYLISTS: push(L_PLAYLISTS, 0); break;
        case M_RADIO: push(L_RADIO, 0); break;
        case M_SETTINGS: return MUSIC_SETTINGS;
        case M_NOW: open_now(); break;
        }
        break;
    case L_ALBUMS: push(L_ALBUM, library_albums_sorted(0)[i]); break;
    case L_ARTISTS: push(L_ARTIST, i); break;
    case L_PLAYLISTS: push(L_PLAYLIST, i); break;
    case L_ARTIST:
        if (i == 0) push(L_ARTIST_SONGS, l->id);
        else push(L_ALBUM, lib.artists[l->id].albums[i - 1]);
        break;
    case L_RADIO:
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
    return MUSIC_STAY;
}

static void remove_station(List *l)
{
    int i = l->sel - 1;
    if (i < 0) return;
    if (stations[i].radio) { show_toast("Radio-only stations stay"); return; }
    if (remove_armed == i && remove_t > 0) {
        stations_remove(i);
        remove_armed = -1;
        l->sel = clampi(l->sel, 0, station_count);
        show_toast("Station removed");
        return;
    }
    remove_armed = i;
    remove_t = 2.2f;
    show_toast("Press START again to remove");
}

static void update_now(const Input *in, float dt)
{
    u32 p = in->pressed;
    if (player_song() < 0) { now_open = 0; return; }
    scrub_update(in, dt);
    if (p & PSP_CTRL_CROSS) player_toggle_pause();
    if (p & PSP_CTRL_RTRIGGER) player_next();
    if (p & PSP_CTRL_LTRIGGER) player_prev();
    if ((p & PSP_CTRL_SQUARE) && !player_locked()) {
        player_set_shuffle(!player_shuffle());
        show_toast(player_shuffle() ? "Shuffle on" : "Shuffle off");
    }
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_SELECT | PSP_CTRL_TRIANGLE)) now_open = 0;
}

int pod_update(const Input *in, float dt)
{
    t_now += dt;
    if (toast_t > 0) toast_t -= dt;
    if (remove_t > 0) remove_t -= dt;
    u32 n = in->nav, p = in->pressed;
    if (library_state() != LIB_READY) return (p & PSP_CTRL_CIRCLE) ? MUSIC_HOME : MUSIC_STAY;
    if (now_open) { update_now(in, dt); return MUSIC_STAY; }

    if (p & (PSP_CTRL_SELECT | PSP_CTRL_TRIANGLE)) { open_now(); return MUSIC_STAY; }
    if ((p & PSP_CTRL_SQUARE) && player_song() >= 0) player_toggle_pause();

    List *l = top();
    int count = list_count(l);
    l->sel = clampi(l->sel, 0, count ? count - 1 : 0);
    if (count) {
        if (n & PSP_CTRL_UP) l->sel = clampi(l->sel - 1, 0, count - 1);
        if (n & PSP_CTRL_DOWN) l->sel = clampi(l->sel + 1, 0, count - 1);
        if (n & PSP_CTRL_LTRIGGER) l->sel = clampi(l->sel - ROWS, 0, count - 1);
        if (n & PSP_CTRL_RTRIGGER) l->sel = clampi(l->sel + ROWS, 0, count - 1);
        if ((p & PSP_CTRL_CROSS) || ((n & PSP_CTRL_RIGHT) && row_opens_list(l, l->sel))) {
            int r = select_row(l);
            if (r != MUSIC_STAY) return r;
        }
        if (p & PSP_CTRL_START) {
            if (l->kind == L_RADIO) remove_station(l);
            else if (l->kind == L_ARTIST) {
                const Artist *ar = &lib.artists[l->id];
                player_play_from(FROM_ARTIST, ar->name, ar->songs, ar->song_count, -1, 1);
                now_open = 1;
            } else play_list(l, -1, 1);
        }
    }
    if (p & PSP_CTRL_CIRCLE || ((n & PSP_CTRL_LEFT) && depth > 1)) {
        if (depth == 1) return MUSIC_HOME;
        depth--;
    }
    return MUSIC_STAY;
}

// ---- Drawing -----------------------------------------------------------------

static void draw_chevron(float x, float cy, u32 color)
{
    for (int k = 0; k < 4; k++) {
        gfx_rect(x + k, cy - 4 + k, 2, 2, color);
        gfx_rect(x + k, cy + 3 - k, 2, 2, color);
    }
}

static void draw_title_bar(const char *title, int menu)
{
    gfx_gradient(0, 0, SCREEN_W, TITLE_H, BAR_TOP, BAR_TOP, BAR_BOT, BAR_BOT);
    gfx_rect(0, TITLE_H - 1, SCREEN_W, 1, BORDER);
    Font *f = A(700, 13);
    float base = 16;
    if (menu) {
        float w = font_width(f, "FL/AC classic", 0), x = (SCREEN_W - w) / 2;
        x = font_draw(f, x, base, "FL", TEXT, 0);
        x = font_draw(f, x, base, "/", HL, 0);
        font_draw(f, x, base, "AC classic", TEXT, 0);
    } else {
        float w = font_width(f, title, 0);
        if (w > 300) font_draw_fit(f, 90, base, title, TEXT, 300);
        else font_draw(f, (SCREEN_W - w) / 2, base, title, TEXT, 0);
    }
    if (player_song() >= 0) ui_icon(player_paused() ? ICON_PAUSE : ICON_PLAY, 8, 5, 12, TEXT);
    ui_battery(A(600, 10), SCREEN_W - 8, 11, TEXT);
}

static void draw_radio_icon(float x, float cy, u32 color)
{
    ui_ring(x, cy - 4, 14, 10, 2, color);
    gfx_rect(x + 3, cy - 1, 4, 4, color);
    gfx_rect(x + 9, cy - 1, 3, 1, color);
    gfx_rect(x + 9, cy + 1, 3, 1, color);
    for (int k = 0; k < 4; k++) gfx_rect(x + 3 + k * 2, cy - 6 - k, 2, 2, color);
}

static void draw_list(List *l, float dt)
{
    int count = list_count(l);
    float view_h = ROWS * ROW_H, target = l->scroll, rowy = l->sel * ROW_H, max = count * ROW_H - view_h;
    if (rowy < target) target = rowy;
    if (rowy + ROW_H > target + view_h) target = rowy + ROW_H - view_h;
    if (target > max) target = max;
    if (target < 0) target = 0;
    l->scroll = ui_approach(l->scroll, target, dt, 22);

    int bar = count > ROWS;
    float w = bar ? LIST_W - 9 : LIST_W;
    Font *f = A(600, 14);
    gfx_clip(0, TITLE_H, LIST_W, view_h);
    int first = (int)(l->scroll / ROW_H);
    for (int i = first; i < count && i <= first + ROWS; i++) {
        float y = TITLE_H + i * ROW_H - l->scroll;
        int sel = i == l->sel;
        u32 c = sel ? WHITE : TEXT;
        if (sel) gfx_gradient(0, y, w, ROW_H, HL_TOP, HL_TOP, HL, HL);
        float x = 10;
        if (l->kind == L_RADIO && i > 0) { draw_radio_icon(x, y + ROW_H / 2, c); x += 22; }
        int chevron = row_opens_list(l, i);
        font_draw_fit(f, x, y + 19, row_label(l, i), c, w - x - (chevron ? 22 : 8));
        if (chevron) draw_chevron(w - 14, y + ROW_H / 2, c);
    }
    gfx_unclip();
    if (bar) {
        // The scroll bar along the list's right edge.
        float bx = LIST_W - 8, by = TITLE_H + 3, bh = view_h - 6;
        ui_ring(bx, by, 6, bh, 1, BORDER);
        float th = fmaxf(12, bh * ROWS / count), ty = by + (bh - th) * (max > 0 ? l->scroll / max : 0);
        gfx_gradient(bx + 1, ty + 1, 4, th - 2, HL_TOP, HL_TOP, HL, HL);
    }
}

// Art with a mirrored reflection fading into the white below.
static void draw_cover(const Texture *t, int seed, float x, float y, float size, float refl)
{
    if (t) {
        gfx_tex(t, x, y, size, size, WHITE);
        if (refl > 0) gfx_tex_part(t, 0, t->h, t->w, -t->h * refl / size, x, y + size, size, refl, WHITE);
    } else {
        ui_placeholder_art(x, y, size, 0, seed);
        u32 c0, c1;
        ui_placeholder_colors(seed, &c0, &c1);
        if (refl > 0) gfx_gradient(x, y + size, size, refl, color_lerp(c0, c1, 0.8f), c1, c0, color_lerp(c0, c1, 0.6f));
    }
    if (refl > 0) gfx_gradient(x, y + size, size, refl, HEX(0xFFFFFF, 150), HEX(0xFFFFFF, 150), BG, BG);
}

static void draw_song_cover(int song, int seed, float x, float y, float size, float refl)
{
    draw_cover(song >= 0 ? art_get(song, ART_LARGE) : NULL, seed, x, y, size, refl);
}

static void draw_playlist_cover(const Playlist *pl, float x, float y, float size)
{
    if (pl->art[0]) {
        const Texture *t = art_get_file(pl->art, ART_LARGE);
        if (t) { draw_cover(t, 0, x, y, size, 24); return; }
    }
    int songs[4], n = playlist_collage(pl, songs);
    if (n < 4) {
        draw_song_cover(n ? songs[0] : -1, pl->name[0], x, y, size, 24);
        return;
    }
    float h = size / 2;
    for (int i = 0; i < 4; i++) {
        const Texture *t = art_get(songs[i], ART_SMALL);
        float cx = x + (i % 2) * h, cy = y + (i / 2) * h;
        if (t) gfx_tex(t, cx, cy, h, h, WHITE);
        else ui_placeholder_art(cx, cy, h, 0, songs[i]);
    }
    gfx_gradient(x, y + size, size, 24, HEX(0xFFFFFF, 200), HEX(0xFFFFFF, 200), BG, BG);
}

static void draw_preview(const List *l)
{
    float px = LIST_W + 1, pw = SCREEN_W - px, size = 150, ax = px + (pw - size) / 2, ay = TITLE_H + 12;
    gfx_rect(LIST_W, TITLE_H, 1, HINT_Y - TITLE_H, DIVIDER);
    char line2[96] = "";
    const char *line1 = "";
    int i = l->sel, count = list_count(l);
    int n, from;
    const char *name;
    const int *songs = list_songs(l, &n, &from, &name);
    if (l->kind == L_MENU || !count) {
        int song = player_song();
        if (song >= 0) {
            const Song *s = &lib.songs[song];
            draw_song_cover(song, s->album_id, ax, ay, size, 24);
            gfx_gradient(ax, ay + size - 34, size, 34, HEX(0x000000, 0), HEX(0x000000, 0), HEX(0x000000, 170), HEX(0x000000, 170));
            font_draw(A(700, 10), ax + 8, ay + size - 9, player_paused() ? "Paused" : "Now playing", WHITE, 0);
            line1 = song_title(s);
            snprintf(line2, sizeof(line2), "%s", song_artist(s));
        } else {
            draw_song_cover(-1, 1, ax, ay, size, 24);
            ui_icon(ICON_NOTE, ax + size / 2 - 24, ay + size / 2 - 24, 48, HEX(0xFFFFFF, 220));
            line1 = count ? "Boro" : "Nothing here yet";
            snprintf(line2, sizeof(line2), "%s", count ? "Nothing playing" : "Add music to MUSIC");
        }
    } else if (songs) {
        const Song *s = &lib.songs[songs[i]];
        draw_song_cover(songs[i], s->album_id, ax, ay, size, 24);
        line1 = song_title(s);
        snprintf(line2, sizeof(line2), "%s", song_artist(s));
    } else switch (l->kind) {
    case L_ALBUMS: case L_ARTIST: {
        int album = l->kind == L_ALBUMS ? library_albums_sorted(0)[i] : i ? lib.artists[l->id].albums[i - 1] : -1;
        if (album < 0) {
            const Artist *ar = &lib.artists[l->id];
            draw_song_cover(ar->art_song, l->id, ax, ay, size, 24);
            line1 = ar->name;
            snprintf(line2, sizeof(line2), "%d songs", ar->song_count);
        } else {
            const Album *a = &lib.albums[album];
            draw_song_cover(a->art_song, album, ax, ay, size, 24);
            line1 = a->name;
            snprintf(line2, sizeof(line2), "%s", a->artist);
        }
        break;
    }
    case L_ARTISTS: case L_PICK: {
        int radio = l->kind == L_PICK && i < lib.radio_count;
        int id = l->kind == L_PICK && !radio ? i - lib.radio_count : i;
        int art = radio ? lib.radio[id].art_song : lib.artists[id].art_song;
        draw_song_cover(art, id, ax, ay, size, 24);
        line1 = radio ? lib.radio[id].name : lib.artists[id].name;
        if (radio) snprintf(line2, sizeof(line2), "Radio only \xC2\xB7 %d songs", lib.radio[id].count);
        else snprintf(line2, sizeof(line2), "%d album%s \xC2\xB7 %d songs", lib.artists[id].album_count,
                      lib.artists[id].album_count == 1 ? "" : "s", lib.artists[id].song_count);
        break;
    }
    case L_PLAYLISTS:
        draw_playlist_cover(&playlists[i], ax, ay, size);
        line1 = playlists[i].name;
        snprintf(line2, sizeof(line2), "%d song%s", playlists[i].count, playlists[i].count == 1 ? "" : "s");
        break;
    case L_RADIO:
        if (i == 0) {
            draw_song_cover(-1, 3, ax, ay, size, 24);
            ui_icon(ICON_PLUS, ax + size / 2 - 22, ay + size / 2 - 22, 44, HEX(0xFFFFFF, 230));
            line1 = "New Station";
            snprintf(line2, sizeof(line2), "Pick any artist");
        } else {
            const Station *st = &stations[i - 1];
            draw_song_cover(station_art_song(st), st->id, ax, ay, size, 24);
            line1 = station_name(st);
            snprintf(line2, sizeof(line2), "Radio \xC2\xB7 no skips");
        }
        break;
    }
    float cx = px + pw / 2, ty = ay + size + 40;
    Font *f1 = A(700, 12), *f2 = A(400, 11);
    float w1 = fminf(font_width(f1, line1, 0), pw - 20), w2 = fminf(font_width(f2, line2, 0), pw - 20);
    font_draw_fit(f1, cx - w1 / 2, ty, line1, TEXT, pw - 20);
    font_draw_fit(f2, cx - w2 / 2, ty + 16, line2, MUTED, pw - 20);
}

static void draw_hints(const int *glyphs, const char **labels, int n)
{
    gfx_rect(0, HINT_Y, SCREEN_W, SCREEN_H - HINT_Y, HINT_BG);
    gfx_rect(0, HINT_Y, SCREEN_W, 1, DIVIDER);
    Font *f = A(400, 9);
    float x = SCREEN_W - 10, base = SCREEN_H - 4;
    for (int i = n - 1; i >= 0; i--) {
        x -= glyphs[i] >= 0 ? ui_hint_width(f, glyphs[i], labels[i]) : font_width(f, labels[i], 0);
        if (glyphs[i] >= 0) ui_hint(f, x, base, glyphs[i], labels[i], MUTED);
        else font_draw(f, x, base, labels[i], MUTED, 0);
        x -= 12;
    }
}

static void draw_browse(float dt)
{
    List *l = top();
    draw_title_bar(list_title(l), l->kind == L_MENU);
    if (!list_count(l)) {
        static const char *empty[] = { [L_PLAYLISTS] = "Make playlists in FL/AC", [L_ALBUMS] = "No albums yet",
                                       [L_ARTISTS] = "No artists yet", [L_SONGS] = "No songs yet" };
        const char *msg = l->kind < (int)(sizeof(empty) / sizeof(empty[0])) && empty[l->kind] ? empty[l->kind] : "Empty";
        font_draw(A(400, 12), 12, TITLE_H + 22, msg, MUTED, 0);
    } else draw_list(l, dt);
    draw_preview(l);

    int n, from;
    const char *name;
    int song_list = list_songs(l, &n, &from, &name) != NULL || l->kind == L_ARTIST;
    const char *back = depth == 1 ? "Home" : "Back";
    if (song_list) {
        draw_hints((const int[]){ ICON_CROSS, -1, ICON_CIRCLE }, (const char *[]){ "Play", "START Shuffle", back }, 3);
    } else if (l->kind == L_RADIO && l->sel > 0 && !stations[l->sel - 1].radio) {
        draw_hints((const int[]){ ICON_CROSS, -1, ICON_CIRCLE }, (const char *[]){ "Play", "START Remove", back }, 3);
    } else if (player_song() >= 0) {
        draw_hints((const int[]){ ICON_CROSS, ICON_SQUARE, ICON_TRIANGLE, ICON_CIRCLE },
                   (const char *[]){ "Select", player_paused() ? "Play" : "Pause", "Now Playing", back }, 4);
    } else {
        draw_hints((const int[]){ ICON_CROSS, ICON_CIRCLE }, (const char *[]){ "Select", back }, 2);
    }
}

static void draw_now(void)
{
    int song = player_song();
    if (song < 0) return;
    const Song *s = &lib.songs[song];
    draw_title_bar("Now Playing", 0);
    float size = 150, ax = 20, ay = TITLE_H + 14;
    draw_song_cover(song, s->album_id, ax, ay, size, 30);

    float x = 190, w = SCREEN_W - 16 - x;
    char line[96];
    int kind;
    const char *source = player_source(&kind);
    if (player_locked()) {
        font_draw_fit(A(700, 11), x, 50, source, HL, w);
    } else {
        int total, i = player_index(&total);
        snprintf(line, sizeof(line), "%d of %d", i + 1, total);
        font_draw(A(400, 11), x, 50, line, MUTED, 0);
    }
    font_draw_fit(A(700, 17), x, 80, song_title(s), TEXT, w);
    font_draw_fit(A(600, 13), x, 100, song_artist(s), TEXT, w);
    font_draw_fit(A(400, 13), x, 118, song_album(s), MUTED, w);

    if (player_locked()) {
        font_draw(A(400, 11), x, 190, "Radio plays on with no skips.", MUTED, 0);
        draw_hints((const int[]){ ICON_CROSS, ICON_CIRCLE }, (const char *[]){ player_paused() ? "Play" : "Pause", "Menu" }, 2);
        return;
    }
    u32 pos = scrub_position(), dur = s->duration_ms;
    float prog = dur ? (float)pos / dur : 0;
    if (prog > 1) prog = 1;
    float py = 176;
    u32 edge = HEX(0x8E9298, 255);
    gfx_rect(x, py, w, 1, edge);
    gfx_rect(x, py + 9, w, 1, edge);
    gfx_rect(x, py, 1, 10, edge);
    gfx_rect(x + w - 1, py, 1, 10, edge);
    if (prog > 0) gfx_gradient(x + 1, py + 1, (w - 2) * prog, 8, HL_TOP, HL_TOP, HL, HL);
    char a[16], b[20];
    ui_format_time(a, sizeof(a), pos);
    ui_format_time(b + 1, sizeof(b) - 1, dur > pos ? dur - pos : 0);
    b[0] = '-';
    Font *f = A(600, 11);
    font_draw(f, x, py + 26, a, TEXT, 0);
    font_draw_right(f, x + w, py + 26, b, TEXT);
    snprintf(line, sizeof(line), "Shuffle: %s", player_shuffle() ? "On" : "Off");
    font_draw(A(400, 11), x, py + 50, line, MUTED, 0);

    const int g[] = { ICON_CROSS, -1, ICON_SQUARE, ICON_CIRCLE };
    const char *lb[] = { player_paused() ? "Play" : "Pause", "L/R Skip", "Shuffle", "Menu" };
    draw_hints(g, lb, 4);
}

static void draw_toast(void)
{
    if (toast_t <= 0) return;
    float a = fminf(1.0f, toast_t / 0.3f) * fminf(1.0f, (2.2f - toast_t) / 0.15f);
    Font *f = A(600, 11);
    float w = font_width(f, toast, 0) + 24, x = (SCREEN_W - w) / 2, y = HINT_Y - 36 + 6 * (1 - a);
    ui_rrect(x, y, w, 24, 6, RGBA(30, 30, 34, (int)(225 * a)));
    font_draw(f, x + 12, y + 16, toast, color_alpha(WHITE, a), 0);
}

void pod_draw(float dt)
{
    gfx_begin(BG);
    if (library_state() != LIB_READY) {
        draw_title_bar("", 1);
        char msg[64];
        snprintf(msg, sizeof(msg), "Loading music\xE2\x80\xA6 %d files", library_scan_count());
        font_draw_center(A(600, 13), SCREEN_W / 2, SCREEN_H / 2, msg, MUTED);
    } else if (now_open && player_song() >= 0) {
        draw_now();
    } else {
        draw_browse(dt);
    }
    draw_toast();
    ui_volume(A(700, 10), HEX(0xF4F5F7, 245), DIVIDER, HL, TEXT);
    gfx_end();
}
