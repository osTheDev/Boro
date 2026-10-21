// FL/AC theme: Spotify-style sidebar, home with rotating suggestions, album
// grid, artist, song and playlist lists, radio stations, album, artist and
// playlist pages, now playing and the queue. Playlists are named with an
// on-screen keyboard and songs are added by holding triangle.
//
// This file holds the shared state, navigation and the parts every screen
// draws (sidebar, mini player, headers, hints); the screens themselves live in
// flac_browse.c, flac_pages.c and flac_playlists.c.
#include "flac_internal.h"

static const char *section_names[SEC_COUNT] = { "Home", "Albums", "Artists", "Songs", "Playlists", "Radio" };
int section, sidebar_focus = 1;
static int views[8], depth; // view stack above the section view
float t_now;
static char toast[160];
static float toast_t;

// Home
int slide_count, slide_i;
float slide_t;
int home_sel; // 0: carousel, 1..4: jump back in

// Lists
int album_sel, album_sort, artist_sel, song_sel;

// Album / artist pages
int page_album, page_artist, page_sel, page_pane, page_album_sel;
float page_scroll;

// Playlists, radio, artist picker and queue
int playlist_sel, page_playlist, station_sel, pick_sel, queue_sel;
float playlist_scroll, station_scroll, pick_scroll, queue_scroll;
float remove_t;
int queue_buf[QUEUE_MAX], queue_len, queue_first;

// Triangle: tap and hold do different things.
static float tri_t;
static int tri_fired;

// Modals
int modal, add_song, add_sel, menu_sel;
char kb_text[PLAYLIST_NAME_MAX + 1];
int kb_mode, kb_song, kb_row, kb_col, kb_caps;

Font *F(int weight, int px) { return ui_font(FAMILY_MANROPE, weight, px); }

void show_toast(const char *msg)
{
    snprintf(toast, sizeof(toast), "%s", msg);
    toast_t = 2.2f;
}

static int view(void) { return depth ? views[depth - 1] : VIEW_SECTION; }

void push(int v) { if (depth < 8) views[depth++] = v; }

void pop(void) { if (depth) depth--; }

u32 darken(u32 c, float k)
{
    return RGBA((int)((c & 0xFF) * k), (int)(((c >> 8) & 0xFF) * k), (int)(((c >> 16) & 0xFF) * k), 255);
}

// Art for a song, or a placeholder gradient seeded by the album.
void draw_art(int song, int kind, float x, float y, float size, float r, int seed)
{
    const Texture *t = art_get(song, kind);
    if (t) gfx_tex(t, x, y, size, size, WHITE);
    else ui_placeholder_art(x, y, size, r, seed);
}

u32 art_tint(int song, int seed)
{
    u32 c0, c1;
    ui_placeholder_colors(seed, &c0, &c1);
    return art_color(song, color_lerp(c0, c1, 0.5f));
}

float follow(float scroll, int sel, float row_h, float view_h, int count, float dt)
{
    float target = scroll, top = sel * row_h, max = count * row_h - view_h;
    if (top < target) target = top;
    if (top + row_h > target + view_h) target = top + row_h - view_h;
    if (target > max) target = max;
    if (target < 0) target = 0;
    return ui_approach(scroll, target, dt, 18);
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void play_album(int album, int start, int shuffle)
{
    const Album *a = &lib.albums[album];
    play_from(FROM_ALBUM, a->name, a->songs, a->count, start, shuffle);
}

void play_in_album(int song)
{
    const Album *a = &lib.albums[lib.songs[song].album_id];
    for (int i = 0; i < a->count; i++)
        if (a->songs[i] == song) play_album(lib.songs[song].album_id, i, 0);
}

void play_station(int index)
{
    if (!station_play(index)) { show_toast("This station has no songs"); return; }
    push(VIEW_NOW);
}

void open_playlist(int index)
{
    page_playlist = index;
    page_sel = 0;
    page_scroll = 0;
    push(VIEW_PLAYLIST);
}

void open_keyboard(int mode, const char *text, int song)
{
    modal = MODAL_KEYBOARD;
    kb_mode = mode;
    kb_song = song;
    kb_row = 1;
    kb_col = 0;
    snprintf(kb_text, sizeof(kb_text), "%s", text);
    kb_caps = !text[0];
}

static void open_add(int song)
{
    if (song < 0) return;
    if (lib.songs[song].radio) { show_toast("Radio-only songs stay on the radio"); return; }
    modal = MODAL_ADD;
    add_song = song;
    add_sel = 0;
}

void open_album(int album)
{
    page_album = album;
    page_sel = 0;
    page_scroll = 0;
    push(VIEW_ALBUM);
}

void open_artist(int artist)
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

// The song a held triangle would add to a playlist, or -1.
static int focused_song(void)
{
    switch (view()) {
    case VIEW_SECTION:
        return section == SEC_SONGS && !sidebar_focus && lib.songs_az_count ? lib.songs_az[song_sel] : -1;
    case VIEW_ALBUM: return lib.albums[page_album].songs[page_sel];
    case VIEW_ARTIST: return page_pane == 0 ? lib.artists[page_artist].songs[page_sel] : -1;
    case VIEW_PLAYLIST: {
        const Playlist *pl = &playlists[page_playlist];
        return pl->count ? pl->songs[page_sel] : -1;
    }
    case VIEW_NOW: return player_song();
    case VIEW_QUEUE: return queue_len ? queue_buf[queue_sel] : -1;
    }
    return -1;
}

static void open_now(void)
{
    if (player_song() >= 0) push(VIEW_NOW);
    else show_toast("Nothing is playing");
}

static void tap_triangle(void)
{
    switch (view()) {
    case VIEW_SECTION:
        if (section == SEC_RADIO && !sidebar_focus && station_sel > 0) tap_station();
        else open_now();
        break;
    case VIEW_ARTIST: {
        int i = stations_add(0, page_artist);
        play_station(i);
        break;
    }
    case VIEW_PLAYLIST: modal = MODAL_MENU; menu_sel = 0; break;
    case VIEW_NOW: queue_sel = 0; queue_scroll = 0; push(VIEW_QUEUE); break;
    case VIEW_QUEUE: pop(); break;
    default: open_now(); break;
    }
}

int flac_update(const Input *in, float dt)
{
    t_now += dt;
    if (toast_t > 0) toast_t -= dt;
    if (remove_t > 0) remove_t -= dt;
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

    switch (modal) {
    case MODAL_ADD: update_add(in); return 0;
    case MODAL_KEYBOARD: update_keyboard(in); return 0;
    case MODAL_MENU: update_menu(in); return 0;
    case MODAL_DELETE: update_delete(in, dt); return 0;
    }

    int v = view();
    // Triangle: a tap opens now playing (or the screen's own action), holding
    // it adds the focused song to a playlist. Taps act on release.
    if (in->held & PSP_CTRL_TRIANGLE) {
        tri_t += dt;
        if (!tri_fired && tri_t >= HOLD_SECONDS && focused_song() >= 0) {
            tri_fired = 1;
            open_add(focused_song());
            return 0;
        }
    } else if (tri_t > 0) {
        int tap = !tri_fired;
        tri_t = 0;
        tri_fired = 0;
        if (tap) { tap_triangle(); return 0; }
    }
    if (v != VIEW_NOW && v != VIEW_QUEUE && (in->pressed & PSP_CTRL_SELECT)) {
        open_now();
        return 0;
    }
    // □ plays and pauses from every screen except now playing, where it shuffles.
    if (v != VIEW_NOW && (in->pressed & PSP_CTRL_SQUARE) && player_song() >= 0) player_toggle_pause();
    switch (v) {
    case VIEW_SECTION:
        if (sidebar_focus && (in->pressed & PSP_CTRL_CIRCLE)) return 1;
        update_section(in);
        break;
    case VIEW_ALBUM: update_album_page(in); break;
    case VIEW_ARTIST: update_artist_page(in); break;
    case VIEW_NOW: update_now(in, dt); break;
    case VIEW_PLAYLIST: update_playlist_page(in); break;
    case VIEW_QUEUE: update_queue(in); break;
    case VIEW_PICK_ARTIST: update_pick_artist(in); break;
    }
    return 0;
}

void draw_logo(float x, float baseline)
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
        font_draw_fit(fa, 46, BAR_Y + 31, player_locked() ? player_source(&(int){0}) : song_artist(s), GRAY, 330);
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

void draw_header(const char *title, int size, const char *right_hint)
{
    Font *f = F(800, size);
    font_draw(f, CONTENT_X, 10 + font_ascent(f) * 0.95f, title, WHITE, 0);
    Font *small = F(500, 9);
    float right = SCREEN_W - 12;
    ui_battery(small, right, 17, GRAY);
    if (right_hint) {
        char hint[48];
        snprintf(hint, sizeof(hint), "START %s", right_hint);
        font_draw(small, right - 62 - font_width(small, hint, 0), 20, hint, GRAY, 0);
    }
}

void draw_hints(float right, float baseline, const int *glyphs, const char **labels, int n, Font *f)
{
    float x = right;
    for (int i = n - 1; i >= 0; i--) {
        x -= glyphs[i] >= 0 ? ui_hint_width(f, glyphs[i], labels[i]) : font_width(f, labels[i], 0);
        if (glyphs[i] >= 0) ui_hint(f, x, baseline, glyphs[i], labels[i], GRAY);
        else font_draw(f, x, baseline, labels[i], GRAY, 0);
        x -= 12;
    }
}

void draw_page_header(int art_song, int seed, int round, const char *kicker, const char *title)
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

void draw_pills(const char *primary, const char *secondary)
{
    Font *f = F(800, 10);
    float w1 = font_width(f, primary, 0) + 24, w2 = font_width(f, secondary, 0) + 22;
    float x2 = SCREEN_W - 14 - w2, x1 = x2 - 6 - w1, y = 60;
    ui_rrect(x1, y, w1, 24, 12, ACCENT);
    font_draw(f, x1 + 12, y + 16, primary, HEX(0x000000, 255), 0);
    ui_ring(x2, y, w2, 24, 12, WHITE);
    font_draw(f, x2 + 11, y + 16, secondary, WHITE, 0);
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
        case SEC_PLAYLISTS: draw_playlists(dt); break;
        case SEC_RADIO: draw_radio(dt); break;
        }
        draw_sidebar();
        draw_mini_player();
        break;
    case VIEW_ALBUM: draw_album_page(dt); break;
    case VIEW_ARTIST: draw_artist_page(dt); break;
    case VIEW_NOW: draw_now(); break;
    case VIEW_PLAYLIST: draw_playlist_page(dt); break;
    case VIEW_QUEUE: draw_queue(dt); break;
    case VIEW_PICK_ARTIST: draw_pick_artist(dt); break;
    }
    switch (modal) {
    case MODAL_ADD: draw_add(); break;
    case MODAL_KEYBOARD: draw_keyboard(); break;
    case MODAL_MENU: draw_menu(); break;
    case MODAL_DELETE: draw_delete(); break;
    }
    draw_toast();
    ui_volume(F(700, 10), HEX(0x282828, 245), HEX(0x535353, 255), ACCENT, WHITE);
    gfx_end();
}
