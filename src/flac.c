// FL/AC theme: Spotify-style sidebar, home with rotating suggestions, album
// grid, artist and song lists, album and artist pages, and now playing.
#include "flac.h"

#include <math.h>
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "library.h"
#include "player.h"
#include "sys.h"
#include "ui.h"

#define BG HEX(0x121212, 255)
#define SIDEBAR HEX(0x000000, 255)
#define CARD HEX(0x181818, 255)
#define CARD_SEL HEX(0x282828, 255)
#define ROW_SEL HEX(0x2A2A2A, 255)
#define WHITE HEX(0xFFFFFF, 255)
#define GRAY HEX(0xB3B3B3, 255)
#define LIGHT HEX(0xE0E0E0, 255)
#define DIM HEX(0x4D4D4D, 255)
#define ACCENT HEX(0x2BD97C, 255)

#define SIDE_W 92
#define BAR_H 40
#define BAR_Y (SCREEN_H - BAR_H)
#define CONTENT_X (SIDE_W + 12)
#define CONTENT_W (SCREEN_W - SIDE_W - 24)
#define SLIDE_SECONDS 4.0f

enum { SEC_HOME, SEC_ALBUMS, SEC_ARTISTS, SEC_SONGS, SEC_RADIO, SEC_COUNT };
enum { VIEW_SECTION, VIEW_ALBUM, VIEW_ARTIST, VIEW_NOW };
enum { SLIDE_ALBUM, SLIDE_SONG, SLIDE_SHUFFLE };

static const char *section_names[SEC_COUNT] = { "Home", "Albums", "Artists", "Songs", "Radio" };

static int section, sidebar_focus = 1;
static int views[8], depth; // view stack above the section view
static float t_now;
static char toast[64];
static float toast_t;

// Home
typedef struct { int kind, id; char kicker[96], title[128], sub[128]; } Slide;
static Slide slides[4];
static int slide_count, slide_i;
static float slide_t;
static u32 tint, tint_target;
static int home_sel; // 0: carousel, 1..4: jump back in
static int jump[4], jump_count;

// Lists
static int album_sel, album_sort, artist_sel, song_sel;
static float album_scroll, artist_scroll, song_scroll;

// Album / artist pages
static int page_album, page_artist, page_sel, page_pane, page_album_sel;
static float page_scroll;

// ---- Small helpers --------------------------------------------------------

static Font *F(int weight, int px) { return ui_font(FAMILY_MANROPE, weight, px); }

static void show_toast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_t = 2.2f;
}

static int view(void) { return depth ? views[depth - 1] : VIEW_SECTION; }
static void push(int v) { if (depth < 8) views[depth++] = v; }
static void pop(void) { if (depth) depth--; }

static u32 darken(u32 c, float k)
{
    return RGBA((int)((c & 0xFF) * k), (int)(((c >> 8) & 0xFF) * k), (int)(((c >> 16) & 0xFF) * k), 255);
}

// Art for a song, or a placeholder gradient seeded by the album.
static void draw_art(int song, int kind, float x, float y, float size, float r, int seed)
{
    const Texture *t = art_get(song, kind);
    if (t) gfx_tex(t, x, y, size, size, WHITE);
    else ui_placeholder_art(x, y, size, r, seed);
}

static u32 art_tint(int song, int seed)
{
    u32 c0, c1;
    ui_placeholder_colors(seed, &c0, &c1);
    return art_color(song, color_lerp(c0, c1, 0.5f));
}

static float follow(float scroll, int sel, float row_h, float view_h, int count, float dt)
{
    float target = scroll, top = sel * row_h, max = count * row_h - view_h;
    if (top < target) target = top;
    if (top + row_h > target + view_h) target = top + row_h - view_h;
    if (target > max) target = max;
    if (target < 0) target = 0;
    return ui_approach(scroll, target, dt, 18);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- Home suggestions ----------------------------------------------------

static void build_slides(void)
{
    slide_count = 0;
    jump_count = library_recent(jump, 4);
    if (!lib.album_count) return;

    // Because you played: another album by the artist of the latest album.
    if (jump_count) {
        const Album *last = &lib.albums[jump[0]];
        const Artist *ar = &lib.artists[last->artist_id];
        for (int i = 0; i < ar->album_count; i++) {
            int a = ar->albums[i];
            if (a == jump[0]) continue;
            Slide *s = &slides[slide_count++];
            s->kind = SLIDE_ALBUM;
            s->id = a;
            snprintf(s->kicker, sizeof(s->kicker), "Because you played %s", ar->name);
            snprintf(s->title, sizeof(s->title), "%s", lib.albums[a].name);
            snprintf(s->sub, sizeof(s->sub), "Album \xC2\xB7 %d songs", lib.albums[a].count);
            break;
        }
    }
    // Rediscover: a random album that isn't in the recent list.
    for (int tries = 0; tries < 8; tries++) {
        int a = rand() % lib.album_count, recent = 0;
        for (int i = 0; i < jump_count; i++) recent |= jump[i] == a;
        if (recent && tries < 7) continue;
        Slide *s = &slides[slide_count++];
        s->kind = SLIDE_ALBUM;
        s->id = a;
        snprintf(s->kicker, sizeof(s->kicker), "Rediscover");
        snprintf(s->title, sizeof(s->title), "%s", lib.albums[a].name);
        snprintf(s->sub, sizeof(s->sub), "%s \xC2\xB7 %d songs", lib.albums[a].artist, lib.albums[a].count);
        break;
    }
    // Deep cut: a random song.
    if (lib.songs_az_count) {
        int song = lib.songs_az[rand() % lib.songs_az_count];
        const Song *x = &lib.songs[song];
        Slide *s = &slides[slide_count++];
        s->kind = SLIDE_SONG;
        s->id = song;
        snprintf(s->kicker, sizeof(s->kicker), "Deep cut");
        snprintf(s->title, sizeof(s->title), "%s", song_title(x));
        snprintf(s->sub, sizeof(s->sub), "%s \xC2\xB7 from %s", song_artist(x), song_album(x));
    }
    // Shuffle everything.
    Slide *s = &slides[slide_count++];
    s->kind = SLIDE_SHUFFLE;
    s->id = lib.songs_az_count ? lib.songs_az[rand() % lib.songs_az_count] : -1;
    snprintf(s->kicker, sizeof(s->kicker), "Mix it up");
    snprintf(s->title, sizeof(s->title), "Shuffle your library");
    snprintf(s->sub, sizeof(s->sub), "%d songs \xC2\xB7 %d albums", lib.songs_az_count, lib.album_count);
    slide_i = 0;
    slide_t = 0;
}

static int slide_art_song(const Slide *s)
{
    if (s->kind == SLIDE_ALBUM) {
        int a = lib.albums[s->id].art_song;
        return a >= 0 ? a : lib.albums[s->id].songs[0];
    }
    return s->id;
}

static int slide_seed(const Slide *s)
{
    return s->kind == SLIDE_ALBUM ? s->id : s->id >= 0 ? lib.songs[s->id].album_id : 0;
}

// ---- Actions ---------------------------------------------------------------

static void play_album(int album, int start, int shuffle)
{
    const Album *a = &lib.albums[album];
    player_play(a->songs, a->count, start, shuffle);
}

static void play_in_album(int song)
{
    const Album *a = &lib.albums[lib.songs[song].album_id];
    for (int i = 0; i < a->count; i++)
        if (a->songs[i] == song) player_play(a->songs, a->count, i, 0);
}

static void open_album(int album)
{
    page_album = album;
    page_sel = 0;
    page_scroll = 0;
    push(VIEW_ALBUM);
}

static void open_artist(int artist)
{
    page_artist = artist;
    page_sel = page_album_sel = page_pane = 0;
    page_scroll = 0;
    push(VIEW_ARTIST);
}

void flac_enter(void)
{
    depth = 0;
    sidebar_focus = 1;
    section = SEC_HOME;
    home_sel = 0;
    if (library_state() == LIB_READY) build_slides();
}

// ---- Update ----------------------------------------------------------------

static void update_home(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    if (n & PSP_CTRL_UP) home_sel = home_sel > 2 ? home_sel - 2 : 0;
    if (n & PSP_CTRL_DOWN) {
        if (home_sel == 0 && jump_count) home_sel = 1;
        else if (home_sel && home_sel + 2 <= jump_count) home_sel += 2;
    }
    if (n & PSP_CTRL_LEFT) {
        if (home_sel == 2 || home_sel == 4) home_sel--;
        else sidebar_focus = 1;
    }
    if (n & PSP_CTRL_RIGHT) {
        if (home_sel == 0 && slide_count) { slide_i = (slide_i + 1) % slide_count; slide_t = 0; }
        else if ((home_sel == 1 || home_sel == 3) && home_sel + 1 <= jump_count) home_sel++;
    }
    if (p & PSP_CTRL_CROSS) {
        if (home_sel == 0 && slide_count) {
            const Slide *s = &slides[slide_i];
            if (s->kind == SLIDE_ALBUM) open_album(s->id);
            else if (s->kind == SLIDE_SONG) { play_in_album(s->id); push(VIEW_NOW); }
            else { player_play(lib.songs_az, lib.songs_az_count, -1, 1); push(VIEW_NOW); }
        } else if (home_sel > 0 && home_sel <= jump_count) {
            open_album(jump[home_sel - 1]);
        }
    }
}

static void update_albums(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    int count = lib.album_count;
    if (!count) { if (n & PSP_CTRL_LEFT) sidebar_focus = 1; return; }
    if (n & PSP_CTRL_LEFT) { if (album_sel % 4 == 0) sidebar_focus = 1; else album_sel--; }
    if (n & PSP_CTRL_RIGHT && album_sel + 1 < count && album_sel % 4 != 3) album_sel++;
    if (n & PSP_CTRL_UP && album_sel >= 4) album_sel -= 4;
    if (n & PSP_CTRL_DOWN && album_sel + 4 < count) album_sel += 4;
    if (n & PSP_CTRL_LTRIGGER) album_sel = clampi(album_sel - 8, 0, count - 1);
    if (n & PSP_CTRL_RTRIGGER) album_sel = clampi(album_sel + 8, 0, count - 1);
    if (p & PSP_CTRL_SQUARE) {
        int id = library_albums_sorted(album_sort)[album_sel];
        album_sort = !album_sort;
        int *order = library_albums_sorted(album_sort);
        for (int i = 0; i < count; i++)
            if (order[i] == id) album_sel = i;
        show_toast(album_sort ? "Sorted by artist" : "Sorted A\xE2\x80\x93Z");
    }
    if (p & PSP_CTRL_CROSS) open_album(library_albums_sorted(album_sort)[album_sel]);
}

static void update_list(const Input *in, int *sel, int count, int page)
{
    u32 n = in->nav;
    if (n & PSP_CTRL_LEFT) sidebar_focus = 1;
    if (!count) return;
    if (n & PSP_CTRL_UP) *sel = clampi(*sel - 1, 0, count - 1);
    if (n & PSP_CTRL_DOWN) *sel = clampi(*sel + 1, 0, count - 1);
    if (n & PSP_CTRL_LTRIGGER) *sel = clampi(*sel - page, 0, count - 1);
    if (n & PSP_CTRL_RTRIGGER) *sel = clampi(*sel + page, 0, count - 1);
}

static void update_section(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    if (sidebar_focus) {
        if (n & PSP_CTRL_UP && section > 0) section--;
        if (n & PSP_CTRL_DOWN && section < SEC_COUNT - 1) section++;
        if (p & (PSP_CTRL_CROSS | PSP_CTRL_RIGHT)) sidebar_focus = 0;
        if (section == SEC_HOME && (n & (PSP_CTRL_UP | PSP_CTRL_DOWN))) build_slides();
        return;
    }
    if (p & PSP_CTRL_CIRCLE) { sidebar_focus = 1; return; }
    switch (section) {
    case SEC_HOME: update_home(in); break;
    case SEC_ALBUMS: update_albums(in); break;
    case SEC_ARTISTS:
        update_list(in, &artist_sel, lib.artist_count, 5);
        if (p & PSP_CTRL_CROSS && lib.artist_count) open_artist(artist_sel);
        break;
    case SEC_SONGS:
        update_list(in, &song_sel, lib.songs_az_count, 6);
        if (p & PSP_CTRL_CROSS && lib.songs_az_count) player_play(lib.songs_az, lib.songs_az_count, song_sel, 0);
        if (p & PSP_CTRL_SQUARE && lib.songs_az_count) { player_play(lib.songs_az, lib.songs_az_count, -1, 1); push(VIEW_NOW); }
        break;
    case SEC_RADIO:
        if (n & PSP_CTRL_LEFT) sidebar_focus = 1;
        break;
    }
}

static void update_album_page(const Input *in)
{
    const Album *a = &lib.albums[page_album];
    u32 n = in->nav, p = in->pressed;
    if (n & PSP_CTRL_UP) page_sel = clampi(page_sel - 1, 0, a->count - 1);
    if (n & PSP_CTRL_DOWN) page_sel = clampi(page_sel + 1, 0, a->count - 1);
    if (p & PSP_CTRL_CROSS) play_album(page_album, page_sel, 0);
    if (p & PSP_CTRL_SQUARE) { play_album(page_album, -1, 1); push(VIEW_NOW); }
    if (p & PSP_CTRL_CIRCLE) pop();
}

static void update_artist_page(const Input *in)
{
    const Artist *ar = &lib.artists[page_artist];
    u32 n = in->nav, p = in->pressed;
    if (p & (PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)) page_pane = !page_pane;
    if (page_pane == 0) {
        if (n & PSP_CTRL_UP) page_sel = clampi(page_sel - 1, 0, ar->song_count - 1);
        if (n & PSP_CTRL_DOWN) page_sel = clampi(page_sel + 1, 0, ar->song_count - 1);
        if (n & PSP_CTRL_RIGHT) page_pane = 1;
        if (p & PSP_CTRL_CROSS) player_play(ar->songs, ar->song_count, page_sel, 0);
    } else {
        if (n & PSP_CTRL_LEFT) { if (page_album_sel % 2 == 0) page_pane = 0; else page_album_sel--; }
        if (n & PSP_CTRL_RIGHT && page_album_sel % 2 == 0 && page_album_sel + 1 < ar->album_count) page_album_sel++;
        if (n & PSP_CTRL_UP && page_album_sel >= 2) page_album_sel -= 2;
        if (n & PSP_CTRL_DOWN && page_album_sel + 2 < ar->album_count) page_album_sel += 2;
        if (p & PSP_CTRL_CROSS) open_album(ar->albums[page_album_sel]);
    }
    if (p & PSP_CTRL_SQUARE) { player_play(ar->songs, ar->song_count, -1, 1); push(VIEW_NOW); }
    if (p & PSP_CTRL_TRIANGLE) show_toast("Artist radio arrives in milestone 2");
    if (p & PSP_CTRL_CIRCLE) pop();
}

static void update_now(const Input *in)
{
    u32 p = in->pressed;
    if (p & PSP_CTRL_CROSS) player_toggle_pause();
    if (p & PSP_CTRL_RTRIGGER) player_next();
    if (p & PSP_CTRL_LTRIGGER) player_prev();
    if (p & PSP_CTRL_SQUARE) {
        player_set_shuffle(!player_shuffle());
        show_toast(player_shuffle() ? "Shuffle on" : "Shuffle off");
    }
    if (p & PSP_CTRL_TRIANGLE) show_toast("The queue view arrives in milestone 2");
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_SELECT)) pop();
}

int flac_update(const Input *in, float dt)
{
    t_now += dt;
    if (toast_t > 0) toast_t -= dt;
    if (library_state() != LIB_READY) {
        return (in->pressed & PSP_CTRL_CIRCLE) != 0;
    }
    if (!slide_count && lib.album_count) build_slides();

    // Suggestions rotate every few seconds while the home section is showing.
    if (section == SEC_HOME && view() == VIEW_SECTION && slide_count) {
        slide_t += dt;
        if (slide_t >= SLIDE_SECONDS) {
            slide_t = 0;
            slide_i = (slide_i + 1) % slide_count;
        }
    }

    int v = view();
    // △ / SELECT open now playing (on an artist page △ starts radio instead).
    if (v != VIEW_NOW && (in->pressed & PSP_CTRL_SELECT || (in->pressed & PSP_CTRL_TRIANGLE && v != VIEW_ARTIST))) {
        if (player_song() >= 0) push(VIEW_NOW);
        else show_toast("Nothing is playing");
        return 0;
    }
    switch (v) {
    case VIEW_SECTION:
        if (sidebar_focus && (in->pressed & PSP_CTRL_CIRCLE)) return 1;
        update_section(in);
        break;
    case VIEW_ALBUM: update_album_page(in); break;
    case VIEW_ARTIST: update_artist_page(in); break;
    case VIEW_NOW: update_now(in); break;
    }
    return 0;
}

// ---- Drawing ------------------------------------------------------------------

static void draw_logo(float x, float baseline)
{
    Font *f = F(800, 14);
    float px = font_draw(f, x, baseline, "FL", WHITE, 1);
    px = font_draw(f, px, baseline, "/", ACCENT, 1);
    font_draw(f, px, baseline, "AC", WHITE, 1);
}

static void draw_sidebar(void)
{
    gfx_rect(0, 0, SIDE_W, BAR_Y, SIDEBAR);
    draw_logo(12, 26);
    Font *f = F(700, 11);
    for (int i = 0; i < SEC_COUNT; i++) {
        float y = 37 + i * 29;
        int on = i == section;
        if (on) ui_rrect(8, y, SIDE_W - 16, 25, 4, sidebar_focus ? HEX(0x3A3A3A, 255) : CARD_SEL);
        if (on && sidebar_focus) gfx_rect(8, y + 6, 2, 13, ACCENT);
        float end = font_draw(f, 14, y + 17, section_names[i], on ? WHITE : GRAY, 0);
        if (i == SEC_RADIO) ui_rrect(end + 5, y + 10, 5, 5, 2.5f, ACCENT);
    }
}

static void draw_mini_player(void)
{
    gfx_rect(0, BAR_Y, SCREEN_W, BAR_H, HEX(0x181818, 255));
    gfx_rect(0, BAR_Y, SCREEN_W, 1, CARD_SEL);
    int song = player_song();
    Font *ft = F(700, 10), *fa = F(500, 9);
    if (song >= 0) {
        const Song *s = &lib.songs[song];
        draw_art(song, ART_SMALL, 10, BAR_Y + 6, 28, 3, s->album_id);
        font_draw_fit(ft, 46, BAR_Y + 18, song_title(s), WHITE, 330);
        font_draw_fit(fa, 46, BAR_Y + 31, song_artist(s), GRAY, 330);
        float prog = s->duration_ms ? (float)player_position_ms() / s->duration_ms : 0;
        if (prog > 1) prog = 1;
        gfx_rect(0, BAR_Y - 1, SCREEN_W, 2, HEX(0x404040, 255));
        gfx_rect(0, BAR_Y - 1, SCREEN_W * prog, 2, ACCENT);
    } else {
        ui_rrect(10, BAR_Y + 6, 28, 28, 3, CARD_SEL);
        ui_icon(ICON_NOTE, 16, BAR_Y + 12, 16, GRAY);
        font_draw(ft, 46, BAR_Y + 24, "Nothing playing", GRAY, 0);
    }
    u32 c = song >= 0 && !player_locked() ? WHITE : DIM;
    ui_icon(ICON_PREV, 400, BAR_Y + 13, 14, c);
    ui_rrect(420, BAR_Y + 7, 26, 26, 13, song >= 0 ? WHITE : DIM);
    ui_icon(song >= 0 && !player_paused() ? ICON_PAUSE : ICON_PLAY, 427.5f, BAR_Y + 14.5f, 11, HEX(0x000000, 255));
    ui_icon(ICON_NEXT, 452, BAR_Y + 13, 14, c);
}

static void draw_header(const char *title, int size, const char *right_hint)
{
    Font *f = F(800, size);
    font_draw(f, CONTENT_X, 10 + font_ascent(f) * 0.95f, title, WHITE, 0);
    Font *small = F(500, 9);
    float right = SCREEN_W - 12;
    ui_battery(small, right, 17, GRAY);
    if (right_hint) {
        float w = ui_hint_width(small, ICON_SQUARE, right_hint);
        ui_hint(small, right - 62 - w, 20, ICON_SQUARE, right_hint, GRAY);
    }
}

static const char *greeting(void)
{
    char clock[16];
    sys_clock(clock, sizeof(clock));
    int h = atoi(clock) % 12, pm = strstr(clock, "PM") != NULL;
    int hour = h + (pm ? 12 : 0);
    return hour < 5 ? "Good night" : hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening";
}

static void draw_home(float dt)
{
    if (slide_count) {
        const Slide *s = &slides[slide_i];
        tint_target = darken(art_tint(slide_art_song(s), slide_seed(s)), 0.4f);
    } else {
        tint_target = HEX(0x2A2A2A, 255);
    }
    tint = color_lerp(tint, tint_target, 1.0f - expf(-dt * 3));
    gfx_gradient(SIDE_W, 0, SCREEN_W - SIDE_W, 110, tint, tint, BG, BG);

    draw_header(greeting(), 16, NULL);
    if (!lib.album_count) {
        Font *f = F(700, 11), *g = F(500, 10);
        font_draw(f, CONTENT_X, 70, "No music found yet", WHITE, 0);
        font_draw(g, CONTENT_X, 88, "Copy MP3s into the MUSIC folder on your memory stick.", GRAY, 0);
        return;
    }

    // Carousel.
    float bx = CONTENT_X, by = 36, bw = CONTENT_W, bh = 92;
    ui_rrect(bx, by, bw, bh, 8, HEX(0x1E1E1E, 255));
    if (!sidebar_focus && home_sel == 0) ui_ring(bx, by, bw, bh, 8, ACCENT);
    if (slide_count) {
        const Slide *s = &slides[slide_i];
        float in = slide_t < 0.5f ? slide_t / 0.5f : 1, ease = 1 - (1 - in) * (1 - in);
        float ox = 12 * (1 - ease);
        u32 a = (u32)(255 * ease);
        gfx_clip(bx, by, bw, bh);
        ui_shadow(bx + 8 + ox, by + 8 + 6, 76, 76, 4, 10, RGBA(0, 0, 0, 110 * ease));
        int art = slide_art_song(s);
        if (art_get(art, ART_SMALL)) gfx_tex(art_get(art, ART_SMALL), bx + 8 + ox, by + 8, 76, 76, RGBA(255, 255, 255, a));
        else ui_placeholder_art(bx + 8 + ox, by + 8, 76, 4, slide_seed(s));
        float tx = bx + 96 + ox, tw = bw - 96 - 12;
        Font *fk = F(800, 9), *ft = F(800, 17), *fs = F(500, 10);
        char kicker[96];
        int k = 0;
        for (const char *c = s->kicker; *c && k < 95; c++) kicker[k++] = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
        kicker[k] = 0;
        font_draw_fit(fk, tx, by + 35, kicker, color_alpha(ACCENT, ease), tw);
        font_draw_fit(ft, tx, by + 55, s->title, color_alpha(WHITE, ease), tw);
        font_draw_fit(fs, tx, by + 70, s->sub, color_alpha(GRAY, ease), tw);
        gfx_unclip();
        // Progress dots.
        float dx = bx + bw - 10;
        for (int i = slide_count - 1; i >= 0; i--) {
            float w = i == slide_i ? 16 : 6;
            dx -= w;
            ui_rrect(dx, by + bh - 12, w, 4, 2, DIM);
            if (i == slide_i) ui_rrect(dx, by + bh - 12, fmaxf(4, w * slide_t / SLIDE_SECONDS), 4, 2, WHITE);
            dx -= 4;
        }
    }

    // Jump back in.
    if (jump_count) {
        font_draw(F(800, 11), CONTENT_X, by + bh + 20, "Jump back in", WHITE, 0);
        for (int i = 0; i < jump_count; i++) {
            float w = (CONTENT_W - 6) / 2.0f, x = CONTENT_X + (i % 2) * (w + 6), y = by + bh + 28 + (i / 2) * 32;
            const Album *a = &lib.albums[jump[i]];
            int on = !sidebar_focus && home_sel == i + 1;
            ui_rrect(x, y, w, 26, 4, on ? HEX(0x3A3A3A, 255) : HEX(0x2A2A2A, 255));
            gfx_clip(x, y, w, 26);
            draw_art(a->art_song, ART_SMALL, x, y, 26, 0, jump[i]);
            gfx_unclip();
            font_draw_fit(F(700, 9), x + 32, y + 16, a->name, WHITE, w - 38);
            if (on) ui_ring(x, y, w, 26, 4, ACCENT);
        }
    }
}

static void draw_albums(float dt)
{
    gfx_gradient(SIDE_W, 0, SCREEN_W - SIDE_W, 90, HEX(0x2A2A2A, 255), HEX(0x2A2A2A, 255), BG, BG);
    draw_header("Albums", 18, album_sort ? "Sort: Artist" : "Sort: A\xE2\x80\x93Z");
    int count = lib.album_count;
    if (!count) return;
    int *order = library_albums_sorted(album_sort);
    float gap = 10, cw = (CONTENT_W - 3 * gap) / 4, art = cw - 12, ch = art + 12 + 26, top = 40;
    float view_h = BAR_Y - top;
    album_scroll = follow(album_scroll, album_sel / 4, ch + gap, view_h, (count + 3) / 4, dt);
    gfx_clip(SIDE_W, top - 4, SCREEN_W - SIDE_W, BAR_Y - top + 4);
    int first = (int)(album_scroll / (ch + gap)) * 4;
    for (int i = first; i < count && i < first + 12; i++) {
        float x = CONTENT_X + (i % 4) * (cw + gap), y = top + (i / 4) * (ch + gap) - album_scroll;
        if (y > BAR_Y) break;
        const Album *a = &lib.albums[order[i]];
        int on = !sidebar_focus && i == album_sel;
        ui_rrect(x, y, cw, ch, 6, on ? CARD_SEL : CARD);
        draw_art(a->art_song, ART_SMALL, x + 6, y + 6, art, 4, order[i]);
        font_draw_fit(F(700, 9), x + 6, y + art + 18, a->name, WHITE, art);
        font_draw_fit(F(500, 8), x + 6, y + art + 29, a->artist, GRAY, art);
        if (on) {
            ui_ring(x - 1, y - 1, cw + 2, ch + 2, 7, ACCENT);
            ui_ring(x, y, cw, ch, 6, ACCENT);
        }
    }
    gfx_unclip();
}

static void draw_artists(float dt)
{
    gfx_gradient(SIDE_W, 0, SCREEN_W - SIDE_W, 90, HEX(0x2A2A2A, 255), HEX(0x2A2A2A, 255), BG, BG);
    draw_header("Artists", 18, NULL);
    float top = 40, row = 38;
    artist_scroll = follow(artist_scroll, artist_sel, row, BAR_Y - top - 4, lib.artist_count, dt);
    gfx_clip(SIDE_W, top, SCREEN_W - SIDE_W, BAR_Y - top);
    int first = (int)(artist_scroll / row);
    for (int i = first; i < lib.artist_count && i < first + 7; i++) {
        float y = top + i * row - artist_scroll;
        const Artist *ar = &lib.artists[i];
        if (!sidebar_focus && i == artist_sel) ui_rrect(CONTENT_X - 6, y, CONTENT_W + 12, row - 2, 4, ROW_SEL);
        const Texture *t = art_get(ar->art_song, ART_ROUND);
        if (t) gfx_tex(t, CONTENT_X, y + 3, 30, 30, WHITE);
        else {
            u32 c0, c1;
            ui_placeholder_colors(i, &c0, &c1);
            ui_rrect(CONTENT_X, y + 3, 30, 30, 15, color_lerp(c0, c1, 0.5f));
        }
        char sub[64];
        snprintf(sub, sizeof(sub), "%d album%s \xC2\xB7 %d songs", ar->album_count, ar->album_count == 1 ? "" : "s", ar->song_count);
        font_draw_fit(F(700, 11), CONTENT_X + 40, y + 16, ar->name, WHITE, CONTENT_W - 50);
        font_draw(F(500, 9), CONTENT_X + 40, y + 29, sub, GRAY, 0);
    }
    gfx_unclip();
}

static void draw_song_row(int song, float x, float y, float w, int selected, int number)
{
    const Song *s = &lib.songs[song];
    int playing = song == player_song();
    if (selected) ui_rrect(x - 6, y, w + 12, 28, 4, ROW_SEL);
    float tx = x;
    if (number > 0) {
        char n[16];
        snprintf(n, sizeof(n), "%d", number);
        font_draw(F(500, 9), x, y + 18, n, playing ? ACCENT : GRAY, 0);
        tx += 18;
    } else {
        draw_art(song, ART_SMALL, x, y + 2, 24, 2, s->album_id);
        tx += 32;
    }
    char len[16];
    ui_format_time(len, sizeof(len), s->duration_ms);
    Font *fl = F(500, 9);
    float lw = font_width(fl, len, 0);
    font_draw(fl, x + w - lw, y + 18, len, GRAY, 0);
    if (number > 0) {
        font_draw_fit(F(700, 10), tx, y + 18, song_title(s), playing ? ACCENT : WHITE, w - (tx - x) - lw - 10);
    } else {
        char sub[256];
        snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", song_artist(s), song_album(s));
        font_draw_fit(F(700, 10), tx, y + 12, song_title(s), playing ? ACCENT : WHITE, w - (tx - x) - lw - 10);
        font_draw_fit(F(500, 9), tx, y + 24, sub, GRAY, w - (tx - x) - lw - 10);
    }
}

static void draw_songs(float dt)
{
    gfx_gradient(SIDE_W, 0, SCREEN_W - SIDE_W, 90, HEX(0x2A2A2A, 255), HEX(0x2A2A2A, 255), BG, BG);
    draw_header("Songs", 18, "Shuffle all");
    float top = 40, row = 30;
    song_scroll = follow(song_scroll, song_sel, row, BAR_Y - top - 4, lib.songs_az_count, dt);
    gfx_clip(SIDE_W, top, SCREEN_W - SIDE_W, BAR_Y - top);
    int first = (int)(song_scroll / row);
    for (int i = first; i < lib.songs_az_count && i < first + 8; i++)
        draw_song_row(lib.songs_az[i], CONTENT_X, top + i * row - song_scroll, CONTENT_W, !sidebar_focus && i == song_sel, 0);
    gfx_unclip();
}

static void draw_radio(void)
{
    gfx_gradient(SIDE_W, 0, SCREEN_W - SIDE_W, 90, HEX(0x2E2342, 255), HEX(0x2E2342, 255), BG, BG);
    draw_header("Radio", 18, NULL);
    char line[96];
    snprintf(line, sizeof(line), "%d radio-only artist%s found in MUSIC/RADIO", lib.radio_count, lib.radio_count == 1 ? "" : "s");
    font_draw(F(800, 12), CONTENT_X, 70, "Artist stations arrive in milestone 2", WHITE, 0);
    font_draw(F(500, 10), CONTENT_X, 88, line, GRAY, 0);
}

static void draw_hints(float right, float baseline, const int *glyphs, const char **labels, int n, Font *f)
{
    float x = right;
    for (int i = n - 1; i >= 0; i--) {
        x -= glyphs[i] >= 0 ? ui_hint_width(f, glyphs[i], labels[i]) : font_width(f, labels[i], 0);
        if (glyphs[i] >= 0) ui_hint(f, x, baseline, glyphs[i], labels[i], GRAY);
        else font_draw(f, x, baseline, labels[i], GRAY, 0);
        x -= 12;
    }
}

static void draw_page_header(int art_song, int seed, int round, const char *kicker, const char *title)
{
    u32 c = art_tint(art_song, seed);
    u32 c0 = darken(c, 0.55f), c1 = darken(c, 0.95f);
    gfx_gradient(0, 0, SCREEN_W, 92, c0, color_lerp(c0, c1, 0.5f), color_lerp(c0, c1, 0.5f), c1);
    gfx_gradient(0, 28, SCREEN_W, 64, RGBA(18, 18, 18, 0), RGBA(18, 18, 18, 0), RGBA(18, 18, 18, 230), RGBA(18, 18, 18, 230));
    ui_battery(F(500, 9), SCREEN_W - 12, 13, WHITE);
    if (round) {
        const Texture *t = art_get(art_song, ART_ROUND);
        ui_rrect(13, 17, 66, 66, 33, RGBA(255, 255, 255, 50));
        if (t) gfx_tex(t, 14, 18, 64, 64, WHITE);
        else ui_rrect(14, 18, 64, 64, 32, c);
    } else {
        ui_shadow(14, 22, 64, 64, 4, 8, RGBA(0, 0, 0, 120));
        draw_art(art_song, ART_SMALL, 14, 18, 64, 4, seed);
    }
    font_draw(F(500, 9), 90, 40, kicker, LIGHT, 1);
    font_draw_fit(F(800, 24), 90, 64, title, WHITE, 230);
}

static void draw_pills(const char *primary, const char *secondary)
{
    Font *f = F(800, 10);
    float w1 = font_width(f, primary, 0) + 24, w2 = font_width(f, secondary, 0) + 22;
    float x2 = SCREEN_W - 14 - w2, x1 = x2 - 6 - w1, y = 60;
    ui_rrect(x1, y, w1, 24, 12, ACCENT);
    font_draw(f, x1 + 12, y + 16, primary, HEX(0x000000, 255), 0);
    ui_ring(x2, y, w2, 24, 12, WHITE);
    font_draw(f, x2 + 11, y + 16, secondary, WHITE, 0);
}

static void draw_album_page(float dt)
{
    const Album *a = &lib.albums[page_album];
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    draw_page_header(a->art_song >= 0 ? a->art_song : a->songs[0], page_album, 0, "ALBUM", a->name);
    draw_pills("Play", "Shuffle");
    u32 total = 0;
    for (int i = 0; i < a->count; i++) total += lib.songs[a->songs[i]].duration_ms;
    char sub[200];
    snprintf(sub, sizeof(sub), "%s \xC2\xB7 %d songs \xC2\xB7 %u min", a->artist, a->count, (unsigned)((total + 30000) / 60000));
    font_draw_fit(F(500, 10), 90, 80, sub, LIGHT, 230);

    float top = 100, row = 28, bottom = SCREEN_H - 20;
    page_scroll = follow(page_scroll, page_sel, row, bottom - top, a->count, dt);
    gfx_clip(0, top, SCREEN_W, bottom - top);
    int first = (int)(page_scroll / row);
    for (int i = first; i < a->count && i < first + 7; i++) {
        const Song *s = &lib.songs[a->songs[i]];
        draw_song_row(a->songs[i], 20, top + i * row - page_scroll, SCREEN_W - 40, i == page_sel, s->track ? s->track : i + 1);
    }
    gfx_unclip();
    const int g[] = { ICON_CROSS, ICON_SQUARE, ICON_TRIANGLE, ICON_CIRCLE };
    const char *l[] = { "Play", "Shuffle", "Now playing", "Back" };
    draw_hints(SCREEN_W - 14, SCREEN_H - 6, g, l, 4, F(500, 9));
}

static void draw_artist_page(float dt)
{
    const Artist *ar = &lib.artists[page_artist];
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    draw_page_header(ar->art_song >= 0 ? ar->art_song : ar->songs[0], page_artist, 1, "ARTIST", ar->name);
    draw_pills("Shuffle", "Radio");

    // Songs (left) and albums (right), L/R or the d-pad moves between them.
    float top = 100, lx = 14, lw = (SCREEN_W - 28 - 14) * 0.6f, rx = lx + lw + 14, rw = SCREEN_W - 14 - rx;
    font_draw(F(800, 11), lx, top + 10, "Songs", page_pane == 0 ? WHITE : GRAY, 0);
    font_draw(F(800, 11), rx, top + 10, "Albums", page_pane == 1 ? WHITE : GRAY, 0);
    float list_top = top + 16, row = 22, bottom = SCREEN_H - 20;
    page_scroll = follow(page_scroll, page_sel, row, bottom - list_top, ar->song_count, dt);
    gfx_clip(0, list_top, rx - 4, bottom - list_top);
    int first = (int)(page_scroll / row);
    for (int i = first; i < ar->song_count && i < first + 8; i++) {
        int song = ar->songs[i], playing = song == player_song();
        const Song *s = &lib.songs[song];
        float y = list_top + i * row - page_scroll;
        if (page_pane == 0 && i == page_sel) ui_rrect(lx, y, lw, row - 2, 4, ROW_SEL);
        char n[16], len[16];
        snprintf(n, sizeof(n), "%d", i + 1);
        ui_format_time(len, sizeof(len), s->duration_ms);
        font_draw(F(500, 9), lx + 6, y + 14, n, playing ? ACCENT : GRAY, 0);
        Font *fl = F(500, 9);
        font_draw_right(fl, lx + lw - 6, y + 14, len, GRAY);
        font_draw_fit(F(700, 10), lx + 24, y + 14, song_title(s), playing ? ACCENT : WHITE, lw - 30 - font_width(fl, len, 0) - 8);
    }
    gfx_unclip();

    float cell = (rw - 8) / 2;
    int row_sel = page_album_sel / 2;
    float scroll = row_sel > 0 ? (row_sel) * (cell + 18) : 0;
    gfx_clip(rx - 2, list_top, rw + 4, bottom - list_top);
    for (int i = 0; i < ar->album_count; i++) {
        float x = rx + (i % 2) * (cell + 8), y = list_top + 2 + (i / 2) * (cell + 18) - scroll;
        if (y > bottom) break;
        const Album *a = &lib.albums[ar->albums[i]];
        draw_art(a->art_song, ART_SMALL, x, y, cell, 4, ar->albums[i]);
        if (page_pane == 1 && i == page_album_sel) ui_ring(x - 2, y - 2, cell + 4, cell + 4, 5, ACCENT);
        font_draw_fit(F(700, 8), x, y + cell + 11, a->name, WHITE, cell);
    }
    gfx_unclip();

    const int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_SQUARE, -1, ICON_CIRCLE };
    const char *l[] = { "Play", "Start radio", "Shuffle", "L/R Songs\xC2\xB7" "Albums", "Back" };
    draw_hints(SCREEN_W - 14, SCREEN_H - 6, g, l, 5, F(500, 9));
}

static void draw_now(void)
{
    int song = player_song();
    if (song < 0) { pop(); return; }
    const Song *s = &lib.songs[song];
    u32 c = darken(art_tint(song, s->album_id), 0.55f);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    gfx_gradient(0, 0, SCREEN_W, SCREEN_H, c, color_lerp(c, BG, 0.55f), color_lerp(c, BG, 0.75f), BG);

    // Art.
    ui_shadow(18, 61 + 10, 150, 150, 6, 18, RGBA(0, 0, 0, 140));
    const Texture *t = art_get(song, ART_LARGE);
    if (t) gfx_tex(t, 18, 61, 150, 150, WHITE);
    else draw_art(song, ART_SMALL, 18, 61, 150, 6, s->album_id);

    float x = 186, w = SCREEN_W - 18 - x;
    Font *f9 = F(500, 9);
    font_draw(f9, x, 27, "PLAYING FROM ALBUM", GRAY, 1);
    float right = SCREEN_W - 18;
    ui_battery(f9, right, 23, GRAY);
    if (player_locked()) {
        Font *fb = F(800, 9);
        float bw = font_width(fb, "RADIO \xC2\xB7 NO SKIPS", 0) + 12, bx = right - 52 - bw;
        ui_rrect(bx, 16, bw, 14, 3, ACCENT);
        font_draw(fb, bx + 6, 26, "RADIO \xC2\xB7 NO SKIPS", HEX(0x000000, 255), 0);
    }
    font_draw_fit(F(700, 10), x, 44, song_album(s), LIGHT, w);

    font_draw_fit(F(800, 20), x, 140, song_title(s), WHITE, w);
    font_draw_fit(F(500, 12), x, 158, song_artist(s), GRAY, w);

    // Progress.
    u32 pos = player_position_ms(), dur = s->duration_ms;
    float prog = dur ? (float)pos / dur : 0;
    if (prog > 1) prog = 1;
    float py = 170;
    ui_rrect(x, py, w, 4, 2, DIM);
    ui_rrect(x, py, w * prog, 4, 2, WHITE);
    ui_rrect(x + w * prog - 5, py - 3, 10, 10, 5, WHITE);
    char a[16], b[16];
    ui_format_time(a, sizeof(a), pos);
    ui_format_time(b, sizeof(b), dur);
    font_draw(f9, x, py + 17, a, GRAY, 0);
    font_draw_right(f9, x + w, py + 17, b, GRAY);

    // Controls.
    float cy = 214, mid = x + w / 2;
    u32 skip = player_locked() ? DIM : WHITE;
    ui_icon(ICON_SHUFFLE, x, cy - 8, 16, player_shuffle() ? ACCENT : GRAY);
    if (player_shuffle()) ui_rrect(x + 6, cy + 11, 4, 4, 2, ACCENT);
    ui_icon(ICON_PREV, mid - 58, cy - 10, 20, skip);
    ui_rrect(mid - 20, cy - 20, 40, 40, 20, WHITE);
    ui_icon(player_paused() ? ICON_PLAY : ICON_PAUSE, mid - 8, cy - 8, 16, HEX(0x000000, 255));
    ui_icon(ICON_NEXT, mid + 38, cy - 10, 20, skip);
    ui_icon(ICON_REPEAT, x + w - 16, cy - 8, 16, GRAY);

    const int g[] = { ICON_CROSS, -1, ICON_SQUARE, ICON_TRIANGLE, ICON_CIRCLE };
    const char *l[] = { player_paused() ? "Play" : "Pause", "L/R Skip", "Shuffle", "Queue", "Back" };
    draw_hints(SCREEN_W - 18, SCREEN_H - 12, g, l, 5, F(500, 8));
}

static void draw_scanning(void)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    gfx_gradient(0, 0, SCREEN_W, 140, HEX(0x1E3A26, 255), HEX(0x1E3A26, 255), BG, BG);
    draw_logo(SCREEN_W / 2 - 22, 100);
    char line[64];
    snprintf(line, sizeof(line), "%d songs found", library_scan_count());
    font_draw_center(F(800, 14), SCREEN_W / 2, 140, "Scanning your music", WHITE);
    font_draw_center(F(500, 10), SCREEN_W / 2, 158, line, GRAY);
    for (int i = 0; i < 5; i++) {
        float h = 6 + 10 * (0.5f + 0.5f * sinf(t_now * 6 + i * 0.9f));
        ui_rrect(SCREEN_W / 2 - 22 + i * 9, 190 - h, 5, h, 1.5f, ACCENT);
    }
    font_draw_center(F(500, 9), SCREEN_W / 2, SCREEN_H - 12, "The first scan takes a while. Later launches only read new files.", GRAY);
}

static void draw_toast(void)
{
    if (toast_t <= 0) return;
    float a = fminf(1.0f, toast_t / 0.3f) * fminf(1.0f, (2.2f - toast_t) / 0.15f);
    Font *f = F(700, 10);
    float w = font_width(f, toast, 0) + 24, x = (SCREEN_W - w) / 2, y = BAR_Y - 34 + 6 * (1 - a);
    ui_rrect(x, y, w, 24, 12, RGBA(40, 40, 40, (int)(240 * a)));
    font_draw(f, x + 12, y + 16, toast, color_alpha(WHITE, a), 0);
}

void flac_draw(float dt)
{
    gfx_begin(BG);
    if (library_state() != LIB_READY) {
        draw_scanning();
        gfx_end();
        return;
    }
    switch (view()) {
    case VIEW_SECTION:
        switch (section) {
        case SEC_HOME: draw_home(dt); break;
        case SEC_ALBUMS: draw_albums(dt); break;
        case SEC_ARTISTS: draw_artists(dt); break;
        case SEC_SONGS: draw_songs(dt); break;
        case SEC_RADIO: draw_radio(); break;
        }
        draw_sidebar();
        draw_mini_player();
        break;
    case VIEW_ALBUM: draw_album_page(dt); break;
    case VIEW_ARTIST: draw_artist_page(dt); break;
    case VIEW_NOW: draw_now(); break;
    }
    draw_toast();
    gfx_end();
}
