// FL/AC theme: Spotify-style sidebar, home with rotating suggestions, album
// grid, artist, song and playlist lists, radio stations, album, artist and
// playlist pages, now playing and the queue. Playlists are named with an
// on-screen keyboard and songs are added by holding triangle.
#include "flac.h"

#include <math.h>
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "library.h"
#include "player.h"
#include "playlists.h"
#include "stations.h"
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

#define HOLD_SECONDS 0.5f   // triangle held this long adds the song to a playlist
#define DELETE_SECONDS 2.0f // square held this long deletes a playlist
#define QUEUE_MAX 200

enum { SEC_HOME, SEC_ALBUMS, SEC_ARTISTS, SEC_SONGS, SEC_PLAYLISTS, SEC_RADIO, SEC_COUNT };
enum { VIEW_SECTION, VIEW_ALBUM, VIEW_ARTIST, VIEW_NOW, VIEW_PLAYLIST, VIEW_QUEUE, VIEW_PICK_ARTIST };
enum { SLIDE_ALBUM, SLIDE_SONG, SLIDE_SHUFFLE };
enum { MODAL_NONE, MODAL_ADD, MODAL_KEYBOARD, MODAL_MENU, MODAL_DELETE };
enum { FROM_ALBUM, FROM_ARTIST, FROM_SONGS, FROM_PLAYLIST, FROM_RADIO };
enum { MENU_RENAME, MENU_REMOVE, MENU_DELETE, MENU_COUNT };

static const char *section_names[SEC_COUNT] = { "Home", "Albums", "Artists", "Songs", "Playlists", "Radio" };

static int section, sidebar_focus = 1;
static int views[8], depth; // view stack above the section view
static float t_now;
static char toast[160];
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

// Playlists, radio, artist picker and queue
static int playlist_sel, page_playlist, station_sel, pick_sel, queue_sel;
static float playlist_scroll, station_scroll, pick_scroll, queue_scroll;
static int remove_armed = -1;
static float remove_t;
static int queue_buf[QUEUE_MAX], queue_len, queue_first;

// Where the music was started from, for now playing.
static int from_kind;
static char from_name[128];

// Scrubbing with the analog stick on now playing: the target position while
// the stick is pushed, applied when it lets go.
static int scrubbing; // 1 while the stick is pushed, 2 while the seek lands
static float scrub_ms, settle_t;

// Triangle: tap and hold do different things.
static float tri_t;
static int tri_fired;

// Modals
static int modal, add_song, add_sel, menu_sel;
static float delete_t;
enum { KB_NEW, KB_RENAME };
static char kb_text[PLAYLIST_NAME_MAX + 1];
static int kb_mode, kb_song, kb_row, kb_col, kb_caps;

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

static void play_from(int kind, const char *name, const int *songs, int count, int start, int shuffle)
{
    from_kind = kind;
    snprintf(from_name, sizeof(from_name), "%s", name);
    player_play(songs, count, start, shuffle);
}

static void play_album(int album, int start, int shuffle)
{
    const Album *a = &lib.albums[album];
    play_from(FROM_ALBUM, a->name, a->songs, a->count, start, shuffle);
}

static void play_in_album(int song)
{
    const Album *a = &lib.albums[lib.songs[song].album_id];
    for (int i = 0; i < a->count; i++)
        if (a->songs[i] == song) play_album(lib.songs[song].album_id, i, 0);
}

static void play_station(int index)
{
    const Station *st = &stations[index];
    int *songs, count;
    station_songs(st, &songs, &count);
    if (!count) { show_toast("This station has no songs"); return; }
    char name[128];
    snprintf(name, sizeof(name), "%s Radio", station_name(st));
    play_from(FROM_RADIO, name, songs, count, -1, 1);
    player_set_locked(1);
    push(VIEW_NOW);
}

static void open_playlist(int index)
{
    page_playlist = index;
    page_sel = 0;
    page_scroll = 0;
    push(VIEW_PLAYLIST);
}

static void open_keyboard(int mode, const char *text, int song)
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
            else { play_from(FROM_SONGS, "Your library", lib.songs_az, lib.songs_az_count, -1, 1); push(VIEW_NOW); }
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
    if (p & PSP_CTRL_START) {
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
        if (p & PSP_CTRL_CROSS && lib.songs_az_count) play_from(FROM_SONGS, "All songs", lib.songs_az, lib.songs_az_count, song_sel, 0);
        if (p & PSP_CTRL_START && lib.songs_az_count) { play_from(FROM_SONGS, "All songs", lib.songs_az, lib.songs_az_count, -1, 1); push(VIEW_NOW); }
        break;
    case SEC_PLAYLISTS:
        update_list(in, &playlist_sel, playlist_count + 1, 5);
        if (p & PSP_CTRL_CROSS) {
            if (playlist_sel == 0) open_keyboard(KB_NEW, "", -1);
            else open_playlist(playlist_sel - 1);
        }
        break;
    case SEC_RADIO: {
        int count = station_count + 1;
        if (n & PSP_CTRL_LEFT) { if (station_sel % 4 == 0) sidebar_focus = 1; else station_sel--; }
        if (n & PSP_CTRL_RIGHT && station_sel + 1 < count && station_sel % 4 != 3) station_sel++;
        if (n & PSP_CTRL_UP && station_sel >= 4) station_sel -= 4;
        if (n & PSP_CTRL_DOWN && station_sel + 4 < count) station_sel += 4;
        if (p & PSP_CTRL_CROSS) {
            if (station_sel == 0) { pick_sel = 0; pick_scroll = 0; push(VIEW_PICK_ARTIST); }
            else play_station(station_sel - 1);
        }
        break;
    }
    }
}

// Triangle tapped on a station: press again to remove it.
static void tap_station(void)
{
    int i = station_sel - 1;
    if (i < 0) return;
    if (stations[i].radio) { show_toast("Radio-only stations can't be removed"); return; }
    if (remove_armed == i && remove_t > 0) {
        stations_remove(i);
        remove_armed = -1;
        if (station_sel > station_count) station_sel = station_count;
        show_toast("Station removed");
        return;
    }
    remove_armed = i;
    remove_t = 2.2f;
    show_toast("Press again to remove this station");
}

static void update_playlist_page(const Input *in)
{
    const Playlist *pl = &playlists[page_playlist];
    u32 n = in->nav, p = in->pressed;
    if (pl->count) {
        if (n & PSP_CTRL_UP) page_sel = clampi(page_sel - 1, 0, pl->count - 1);
        if (n & PSP_CTRL_DOWN) page_sel = clampi(page_sel + 1, 0, pl->count - 1);
        if (n & PSP_CTRL_LTRIGGER) page_sel = clampi(page_sel - 6, 0, pl->count - 1);
        if (n & PSP_CTRL_RTRIGGER) page_sel = clampi(page_sel + 6, 0, pl->count - 1);
        if (p & PSP_CTRL_CROSS) play_from(FROM_PLAYLIST, pl->name, pl->songs, pl->count, page_sel, 0);
        if (p & PSP_CTRL_START) { play_from(FROM_PLAYLIST, pl->name, pl->songs, pl->count, -1, 1); push(VIEW_NOW); }
    }
    if (p & PSP_CTRL_CIRCLE) pop();
}

static int pick_count(void) { return lib.radio_count + lib.artist_count; }

static void update_pick_artist(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    int count = pick_count();
    if (count) {
        if (n & PSP_CTRL_UP) pick_sel = clampi(pick_sel - 1, 0, count - 1);
        if (n & PSP_CTRL_DOWN) pick_sel = clampi(pick_sel + 1, 0, count - 1);
        if (n & PSP_CTRL_LTRIGGER) pick_sel = clampi(pick_sel - 5, 0, count - 1);
        if (n & PSP_CTRL_RTRIGGER) pick_sel = clampi(pick_sel + 5, 0, count - 1);
        if (p & PSP_CTRL_CROSS) {
            int radio = pick_sel < lib.radio_count;
            int i = stations_add(radio, radio ? pick_sel : pick_sel - lib.radio_count);
            station_sel = i + 1;
            pop();
            play_station(i);
            return;
        }
    }
    if (p & PSP_CTRL_CIRCLE) pop();
}

static void update_queue(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    queue_len = player_queue(queue_buf, QUEUE_MAX, &queue_first);
    if (queue_len) {
        queue_sel = clampi(queue_sel, 0, queue_len - 1);
        if (n & PSP_CTRL_UP) queue_sel = clampi(queue_sel - 1, 0, queue_len - 1);
        if (n & PSP_CTRL_DOWN) queue_sel = clampi(queue_sel + 1, 0, queue_len - 1);
        if (n & PSP_CTRL_LTRIGGER) queue_sel = clampi(queue_sel - 6, 0, queue_len - 1);
        if (n & PSP_CTRL_RTRIGGER) queue_sel = clampi(queue_sel + 6, 0, queue_len - 1);
        if (p & PSP_CTRL_CROSS) {
            if (player_locked()) show_toast("Radio plays on with no skips");
            else { player_jump(queue_first + queue_sel); queue_sel = 0; }
        }
    }
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_SELECT)) pop();
}

static void update_album_page(const Input *in)
{
    const Album *a = &lib.albums[page_album];
    u32 n = in->nav, p = in->pressed;
    if (n & PSP_CTRL_UP) page_sel = clampi(page_sel - 1, 0, a->count - 1);
    if (n & PSP_CTRL_DOWN) page_sel = clampi(page_sel + 1, 0, a->count - 1);
    if (p & PSP_CTRL_CROSS) play_album(page_album, page_sel, 0);
    if (p & PSP_CTRL_START) { play_album(page_album, -1, 1); push(VIEW_NOW); }
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
        if (p & PSP_CTRL_CROSS) play_from(FROM_ARTIST, ar->name, ar->songs, ar->song_count, page_sel, 0);
    } else {
        if (n & PSP_CTRL_LEFT) { if (page_album_sel % 2 == 0) page_pane = 0; else page_album_sel--; }
        if (n & PSP_CTRL_RIGHT && page_album_sel % 2 == 0 && page_album_sel + 1 < ar->album_count) page_album_sel++;
        if (n & PSP_CTRL_UP && page_album_sel >= 2) page_album_sel -= 2;
        if (n & PSP_CTRL_DOWN && page_album_sel + 2 < ar->album_count) page_album_sel += 2;
        if (p & PSP_CTRL_CROSS) open_album(ar->albums[page_album_sel]);
    }
    if (p & PSP_CTRL_START) { play_from(FROM_ARTIST, ar->name, ar->songs, ar->song_count, -1, 1); push(VIEW_NOW); }
    if (p & PSP_CTRL_CIRCLE) pop();
}

static void update_now(const Input *in, float dt)
{
    u32 p = in->pressed;
    // The stick scrubs: further out moves faster, up to 40 seconds a second.
    int song = player_song(), dx = in->stick_x, dead = 40;
    if (song >= 0 && !player_locked() && (dx > dead || dx < -dead)) {
        if (scrubbing != 1) { scrub_ms = scrubbing ? scrub_ms : player_position_ms(); scrubbing = 1; }
        float k = (fabsf((float)dx) - dead) / (128 - dead);
        scrub_ms += (dx > 0 ? 1 : -1) * k * k * 40000 * dt;
        float dur = lib.songs[song].duration_ms;
        if (scrub_ms < 0) scrub_ms = 0;
        if (scrub_ms > dur - 1000) scrub_ms = dur > 1000 ? dur - 1000 : 0;
    } else if (scrubbing == 1) {
        // Hold the bar at the target until the new position comes through.
        scrubbing = 2;
        settle_t = 0.6f;
        if (song >= 0) player_seek((u32)scrub_ms);
    } else if (scrubbing == 2) {
        settle_t -= dt;
        float off = (float)player_position_ms() - scrub_ms;
        if (settle_t <= 0 || (off > -1500 && off < 1500)) scrubbing = 0;
    }
    if (p & PSP_CTRL_CROSS) player_toggle_pause();
    if (p & PSP_CTRL_RTRIGGER) player_next();
    if (p & PSP_CTRL_LTRIGGER) player_prev();
    if (p & PSP_CTRL_SQUARE) {
        player_set_shuffle(!player_shuffle());
        show_toast(player_shuffle() ? "Shuffle on" : "Shuffle off");
    }
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_SELECT)) pop();
}

// ---- Keyboard ---------------------------------------------------------------

enum { KEY_CHAR, KEY_BACK, KEY_SHIFT, KEY_SPACE };
typedef struct { int kind; char ch; float x, w; } Key;

static const char *kb_rows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
#define KB_KEY_W 36
#define KB_GAP 4

// Lays out one keyboard row, centered on the screen. Returns the key count.
static int kb_layout(int row, Key *keys)
{
    int n = 0;
    if (row == 3) keys[n++] = (Key){ KEY_SHIFT, 0, 0, 44 };
    for (const char *c = kb_rows[row]; *c; c++) keys[n++] = (Key){ KEY_CHAR, *c, 0, KB_KEY_W };
    if (row == 2) keys[n++] = (Key){ KEY_BACK, 0, 0, 52 };
    if (row == 3) keys[n++] = (Key){ KEY_SPACE, ' ', 0, 92 };
    float total = -KB_GAP;
    for (int i = 0; i < n; i++) total += keys[i].w + KB_GAP;
    float x = (SCREEN_W - total) / 2;
    for (int i = 0; i < n; i++) { keys[i].x = x; x += keys[i].w + KB_GAP; }
    return n;
}

static void kb_type(char c)
{
    int len = strlen(kb_text);
    if (len >= PLAYLIST_NAME_MAX) return;
    if (c >= 'A' && c <= 'Z' && !kb_caps) c += 32;
    kb_text[len] = c;
    kb_text[len + 1] = 0;
    if (c != ' ' && len == 0) kb_caps = 0; // capital first letter, then lowercase
}

static void kb_backspace(void)
{
    int len = strlen(kb_text);
    if (len) kb_text[len - 1] = 0;
}

static void kb_save(void)
{
    // Names become file names: trim spaces at both ends.
    char name[PLAYLIST_NAME_MAX + 1];
    const char *start = kb_text;
    while (*start == ' ') start++;
    snprintf(name, sizeof(name), "%s", start);
    int len = strlen(name);
    while (len && name[len - 1] == ' ') name[--len] = 0;
    if (!len) { show_toast("Type a name first"); return; }
    if (kb_mode == KB_NEW) {
        int i = playlist_create(name);
        if (i < 0) { show_toast("There's already a playlist with that name"); return; }
        char msg[64];
        if (kb_song >= 0) {
            playlist_add(i, kb_song);
            snprintf(msg, sizeof(msg), "Added to %s", name);
        } else {
            snprintf(msg, sizeof(msg), "Created %s", name);
            playlist_sel = i + 1;
        }
        show_toast(msg);
    } else {
        int i = playlist_rename(page_playlist, name);
        if (i < 0) { show_toast("There's already a playlist with that name"); return; }
        page_playlist = i;
        show_toast("Renamed");
    }
    modal = MODAL_NONE;
}

static void update_keyboard(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    Key keys[16], other[16];
    int count = kb_layout(kb_row, keys);
    if (n & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        int row = kb_row + ((n & PSP_CTRL_UP) ? -1 : 1);
        if (row >= 0 && row < 4) {
            // Keep the column under the same spot on screen.
            float cx = keys[kb_col].x + keys[kb_col].w / 2, best = 1e9f;
            int m = kb_layout(row, other);
            for (int i = 0; i < m; i++) {
                float d = fabsf(other[i].x + other[i].w / 2 - cx);
                if (d < best) { best = d; kb_col = i; }
            }
            kb_row = row;
            count = m;
            memcpy(keys, other, sizeof(keys));
        }
    }
    if (n & PSP_CTRL_LEFT) kb_col = (kb_col + count - 1) % count;
    if (n & PSP_CTRL_RIGHT) kb_col = (kb_col + 1) % count;
    if (p & PSP_CTRL_CROSS) {
        const Key *k = &keys[kb_col];
        if (k->kind == KEY_CHAR || k->kind == KEY_SPACE) kb_type(k->ch);
        else if (k->kind == KEY_BACK) kb_backspace();
        else kb_caps = !kb_caps;
    }
    if (n & PSP_CTRL_SQUARE) kb_backspace();
    if (p & PSP_CTRL_SELECT) kb_caps = !kb_caps;
    if (p & PSP_CTRL_START) kb_save();
    if (p & PSP_CTRL_CIRCLE) modal = MODAL_NONE;
}

// ---- Modals -------------------------------------------------------------------

static void update_add(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    int count = playlist_count + 1; // "New playlist" first
    if (n & PSP_CTRL_UP) add_sel = clampi(add_sel - 1, 0, count - 1);
    if (n & PSP_CTRL_DOWN) add_sel = clampi(add_sel + 1, 0, count - 1);
    if (p & PSP_CTRL_CROSS) {
        if (add_sel == 0) { open_keyboard(KB_NEW, "", add_song); return; }
        const Playlist *pl = &playlists[add_sel - 1];
        char msg[64];
        snprintf(msg, sizeof(msg), playlist_add(add_sel - 1, add_song) ? "Added to %s" : "Already in %s", pl->name);
        show_toast(msg);
        modal = MODAL_NONE;
    }
    if (p & PSP_CTRL_CIRCLE) modal = MODAL_NONE;
}

static void update_menu(const Input *in)
{
    u32 n = in->nav, p = in->pressed;
    if (n & PSP_CTRL_UP) menu_sel = clampi(menu_sel - 1, 0, MENU_COUNT - 1);
    if (n & PSP_CTRL_DOWN) menu_sel = clampi(menu_sel + 1, 0, MENU_COUNT - 1);
    if (p & PSP_CTRL_CROSS) {
        Playlist *pl = &playlists[page_playlist];
        if (menu_sel == MENU_RENAME) { open_keyboard(KB_RENAME, pl->name, -1); return; }
        if (menu_sel == MENU_REMOVE) {
            if (pl->count) {
                char msg[160];
                snprintf(msg, sizeof(msg), "Removed %s", song_title(&lib.songs[pl->songs[page_sel]]));
                playlist_remove(page_playlist, page_sel);
                if (page_sel >= pl->count && page_sel) page_sel--;
                show_toast(msg);
            }
            modal = MODAL_NONE;
            return;
        }
        modal = MODAL_DELETE;
        delete_t = 0;
        return;
    }
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) modal = MODAL_NONE;
}

static void update_delete(const Input *in, float dt)
{
    if (in->held & PSP_CTRL_SQUARE) {
        delete_t += dt;
        if (delete_t >= DELETE_SECONDS) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Deleted %s", playlists[page_playlist].name);
            playlist_delete(page_playlist);
            modal = MODAL_NONE;
            pop();
            if (playlist_sel > playlist_count) playlist_sel = playlist_count;
            show_toast(msg);
        }
    } else {
        delete_t = 0;
    }
    if (in->pressed & PSP_CTRL_CIRCLE) modal = MODAL_NONE;
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
    static int loaded;
    if (!loaded) {
        loaded = 1;
        stations_load();
        playlists_load();
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
        font_draw_fit(fa, 46, BAR_Y + 31, player_locked() ? from_name : song_artist(s), GRAY, 330);
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
        char hint[48];
        snprintf(hint, sizeof(hint), "START %s", right_hint);
        font_draw(small, right - 62 - font_width(small, hint, 0), 20, hint, GRAY, 0);
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

// A playlist's cover: its own image, one album's art, or a 2x2 collage.
static void draw_playlist_cover(const Playlist *pl, float x, float y, float size, float r)
{
    if (pl->art[0]) {
        const Texture *t = art_get_file(pl->art, ART_SMALL);
        if (t) { gfx_tex(t, x, y, size, size, WHITE); return; }
    }
    int arts[4], n = playlist_collage(pl, arts);
    if (n == 0) {
        ui_rrect(x, y, size, size, r, CARD_SEL);
        ui_icon(ICON_NOTE, x + size * 0.3f, y + size * 0.3f, size * 0.4f, GRAY);
        return;
    }
    if (n == 1) {
        draw_art(arts[0], ART_SMALL, x, y, size, r, lib.songs[arts[0]].album_id);
        return;
    }
    float h = size / 2;
    gfx_clip(x, y, size, size);
    for (int i = 0; i < 4; i++) {
        float qx = x + (i % 2) * h, qy = y + (i / 2) * h;
        if (i < n) draw_art(arts[i], ART_SMALL, qx, qy, h, 0, lib.songs[arts[i]].album_id);
        else gfx_rect(qx, qy, h, h, CARD_SEL);
    }
    gfx_unclip();
}

static u32 playlist_minutes(const Playlist *pl)
{
    u32 total = 0;
    for (int i = 0; i < pl->count; i++) total += lib.songs[pl->songs[i]].duration_ms;
    return (total + 30000) / 60000;
}

static void format_length(char *out, int size, int songs, u32 minutes)
{
    if (minutes >= 60) snprintf(out, size, "%d song%s \xC2\xB7 %u hr %u min", songs, songs == 1 ? "" : "s", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
    else snprintf(out, size, "%d song%s \xC2\xB7 %u min", songs, songs == 1 ? "" : "s", (unsigned)minutes);
}

static void draw_playlists(float dt)
{
    gfx_gradient(SIDE_W, 0, SCREEN_W - SIDE_W, 90, HEX(0x4A2A20, 255), HEX(0x4A2A20, 255), BG, BG);
    draw_header("Playlists", 18, NULL);
    float top = 40, row = 38;
    int count = playlist_count + 1;
    playlist_scroll = follow(playlist_scroll, playlist_sel, row, BAR_Y - top - 4, count, dt);
    gfx_clip(SIDE_W, top, SCREEN_W - SIDE_W, BAR_Y - top);
    int first = (int)(playlist_scroll / row);
    for (int i = first; i < count && i < first + 7; i++) {
        float y = top + i * row - playlist_scroll;
        if (!sidebar_focus && i == playlist_sel) ui_rrect(CONTENT_X - 6, y, CONTENT_W + 12, row - 2, 4, ROW_SEL);
        if (i == 0) {
            ui_rrect(CONTENT_X, y + 3, 30, 30, 4, CARD_SEL);
            ui_icon(ICON_PLUS, CONTENT_X + 7, y + 10, 16, ACCENT);
            font_draw(F(700, 11), CONTENT_X + 40, y + 16, "New playlist", WHITE, 0);
            float hx = font_draw(F(500, 9), CONTENT_X + 40, y + 29, "or hold ", GRAY, 0);
            ui_hint(F(500, 9), hx, y + 29, ICON_TRIANGLE, "on any song", GRAY);
            continue;
        }
        const Playlist *pl = &playlists[i - 1];
        draw_playlist_cover(pl, CONTENT_X, y + 3, 30, 3);
        char sub[64];
        format_length(sub, sizeof(sub), pl->count, playlist_minutes(pl));
        font_draw_fit(F(700, 11), CONTENT_X + 40, y + 16, pl->name, WHITE, CONTENT_W - 50);
        font_draw(F(500, 9), CONTENT_X + 40, y + 29, sub, GRAY, 0);
    }
    gfx_unclip();
}

static void draw_radio(float dt)
{
    gfx_gradient(SIDE_W, 0, SCREEN_W - SIDE_W, 90, HEX(0x3A2347, 255), HEX(0x3A2347, 255), BG, BG);
    draw_header("Radio", 18, NULL);
    Font *small = F(500, 9);
    const char *note = "Shuffled by artist \xC2\xB7 no skips";
    font_draw(small, SCREEN_W - 12 - 62 - font_width(small, note, 0), 20, note, GRAY, 0);

    int count = station_count + 1;
    float gap = 10, cw = (CONTENT_W - 3 * gap) / 4, art = cw - 12, ch = art + 12 + 26, top = 40;
    station_scroll = follow(station_scroll, station_sel / 4, ch + gap, BAR_Y - top, (count + 3) / 4, dt);
    gfx_clip(SIDE_W, top - 4, SCREEN_W - SIDE_W, BAR_Y - top + 4);
    int first = (int)(station_scroll / (ch + gap)) * 4;
    for (int i = first; i < count && i < first + 12; i++) {
        float x = CONTENT_X + (i % 4) * (cw + gap), y = top + (i / 4) * (ch + gap) - station_scroll;
        if (y > BAR_Y) break;
        int on = !sidebar_focus && i == station_sel;
        if (i == 0) {
            ui_rrect(x, y, cw, ch, 6, on ? CARD_SEL : BG);
            ui_ring(x, y, cw, ch, 6, HEX(0x6A6A6A, 255));
            ui_rrect(x + cw / 2 - 15, y + ch / 2 - 26, 30, 30, 15, CARD_SEL);
            ui_icon(ICON_PLUS, x + cw / 2 - 7, y + ch / 2 - 18, 14, ACCENT);
            font_draw_center(F(800, 9), x + cw / 2, y + ch / 2 + 16, "New station", WHITE);
            font_draw_center(F(500, 8), x + cw / 2, y + ch / 2 + 28, "Pick an artist", GRAY);
        } else {
            const Station *st = &stations[i - 1];
            int song = station_art_song(st), seed = st->radio ? 100 + st->id : st->id;
            ui_rrect(x, y, cw, ch, 6, on ? CARD_SEL : CARD);
            draw_art(song, ART_SMALL, x + 6, y + 6, art, 4, seed);
            // Record ring and RADIO badge over the art.
            float cx = x + 6 + art / 2, cy = y + 6 + art / 2, rr = art * 0.3f;
            ui_rrect(cx - rr - 1, cy - rr - 1, rr * 2 + 2, rr * 2 + 2, rr + 1, RGBA(255, 255, 255, 128));
            ui_rrect(cx - rr + 1, cy - rr + 1, rr * 2 - 2, rr * 2 - 2, rr - 1, RGBA(0, 0, 0, 64));
            Font *fb = F(800, 7);
            float bw = font_width(fb, "RADIO", 0) + 8;
            ui_rrect(x + 10, y + 6 + art - 13, bw, 10, 2, HEX(0x000000, 255));
            font_draw(fb, x + 14, y + 6 + art - 5.5f, "RADIO", ACCENT, 0);
            char name[96];
            snprintf(name, sizeof(name), st->radio ? "%s" : "%s Radio", station_name(st));
            font_draw_fit(F(700, 9), x + 6, y + art + 18, name, WHITE, art);
            font_draw(F(500, 8), x + 6, y + art + 29, st->radio ? "Radio only" : "Artist station", GRAY, 0);
        }
        if (on) {
            ui_ring(x - 1, y - 1, cw + 2, ch + 2, 7, ACCENT);
            ui_ring(x, y, cw, ch, 6, ACCENT);
        }
    }
    gfx_unclip();
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
    const int g[] = { ICON_CROSS, -1, ICON_TRIANGLE, ICON_CIRCLE };
    const char *l[] = { "Play", "START Shuffle", "Now playing", "Back" };
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

    const int g[] = { ICON_CROSS, ICON_TRIANGLE, -1, -1, ICON_CIRCLE };
    const char *l[] = { "Play", "Start radio", "START Shuffle", "L/R Songs\xC2\xB7" "Albums", "Back" };
    draw_hints(SCREEN_W - 14, SCREEN_H - 6, g, l, 5, F(500, 9));
}

static void draw_playlist_page(float dt)
{
    const Playlist *pl = &playlists[page_playlist];
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    int arts[4], n = playlist_collage(pl, arts);
    u32 c = n ? art_tint(arts[0], lib.songs[arts[0]].album_id) : HEX(0x8A3A28, 255);
    u32 c0 = darken(c, 0.55f), c1 = darken(c, 0.95f);
    gfx_gradient(0, 0, SCREEN_W, 92, c0, color_lerp(c0, c1, 0.5f), color_lerp(c0, c1, 0.5f), c1);
    gfx_gradient(0, 28, SCREEN_W, 64, RGBA(18, 18, 18, 0), RGBA(18, 18, 18, 0), RGBA(18, 18, 18, 230), RGBA(18, 18, 18, 230));
    ui_battery(F(500, 9), SCREEN_W - 12, 13, WHITE);
    ui_shadow(14, 22, 64, 64, 4, 8, RGBA(0, 0, 0, 120));
    draw_playlist_cover(pl, 14, 18, 64, 4);
    font_draw(F(500, 9), 90, 40, "PLAYLIST", LIGHT, 1);
    font_draw_fit(F(800, 24), 90, 64, pl->name, WHITE, 230);
    char sub[64];
    format_length(sub, sizeof(sub), pl->count, playlist_minutes(pl));
    font_draw(F(500, 10), 90, 80, sub, LIGHT, 0);
    if (pl->count) draw_pills("Play", "Shuffle");

    float top = 100, row = 28, bottom = SCREEN_H - 20;
    if (!pl->count) {
        font_draw(F(800, 12), 20, top + 24, "This playlist is empty", WHITE, 0);
        float x = font_draw(F(500, 10), 20, top + 42, "Hold ", GRAY, 0);
        ui_hint(F(500, 10), x, top + 42, ICON_TRIANGLE, "on any song to add it here.", GRAY);
    }
    page_scroll = follow(page_scroll, page_sel, row, bottom - top, pl->count, dt);
    gfx_clip(0, top, SCREEN_W, bottom - top);
    int first = (int)(page_scroll / row);
    for (int i = first; i < pl->count && i < first + 7; i++)
        draw_song_row(pl->songs[i], 20, top + i * row - page_scroll, SCREEN_W - 40, i == page_sel, 0);
    gfx_unclip();
    const int g[] = { ICON_CROSS, -1, ICON_TRIANGLE, ICON_CIRCLE };
    const char *l[] = { "Play", "START Shuffle", "Options", "Back" };
    draw_hints(SCREEN_W - 14, SCREEN_H - 6, g, l, 4, F(500, 9));
}

static void draw_pick_artist(float dt)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    gfx_gradient(0, 0, SCREEN_W, 90, HEX(0x3A2347, 255), HEX(0x3A2347, 255), BG, BG);
    font_draw(F(800, 18), 20, 28, "New station", WHITE, 0);
    font_draw(F(500, 9), 20, 44, "Pick an artist. Their songs play shuffled, with no skips.", LIGHT, 0);
    ui_battery(F(500, 9), SCREEN_W - 12, 17, GRAY);
    float top = 54, row = 36, bottom = SCREEN_H - 20, x = 20, w = SCREEN_W - 40;
    int count = pick_count();
    pick_scroll = follow(pick_scroll, pick_sel, row, bottom - top, count, dt);
    gfx_clip(0, top, SCREEN_W, bottom - top);
    int first = (int)(pick_scroll / row);
    for (int i = first; i < count && i < first + 7; i++) {
        float y = top + i * row - pick_scroll;
        int radio = i < lib.radio_count, id = radio ? i : i - lib.radio_count;
        const char *name = radio ? lib.radio[id].name : lib.artists[id].name;
        int art = radio ? lib.radio[id].art_song : lib.artists[id].art_song, songs = radio ? lib.radio[id].count : lib.artists[id].song_count;
        if (i == pick_sel) ui_rrect(x - 6, y, w + 12, row - 2, 4, ROW_SEL);
        const Texture *t = art_get(art, ART_ROUND);
        if (t) gfx_tex(t, x, y + 3, 28, 28, WHITE);
        else {
            u32 c0, c1;
            ui_placeholder_colors(radio ? 100 + id : id, &c0, &c1);
            ui_rrect(x, y + 3, 28, 28, 14, color_lerp(c0, c1, 0.5f));
        }
        char sub[64];
        snprintf(sub, sizeof(sub), radio ? "Radio only \xC2\xB7 %d songs" : "%d songs", songs);
        font_draw_fit(F(700, 11), x + 38, y + 15, name, WHITE, w - 48);
        font_draw(F(500, 9), x + 38, y + 28, sub, radio ? ACCENT : GRAY, 0);
    }
    gfx_unclip();
    const int g[] = { ICON_CROSS, ICON_CIRCLE };
    const char *l[] = { "Start station", "Back" };
    draw_hints(SCREEN_W - 14, SCREEN_H - 6, g, l, 2, F(500, 9));
}

static void draw_queue(float dt)
{
    int song = player_song();
    if (song < 0) { pop(); return; }
    queue_len = player_queue(queue_buf, QUEUE_MAX, &queue_first);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    u32 c = darken(art_tint(song, lib.songs[song].album_id), 0.45f);
    gfx_gradient(0, 0, SCREEN_W, 90, c, c, BG, BG);
    font_draw(F(800, 18), 20, 28, "Queue", WHITE, 0);
    ui_battery(F(500, 9), SCREEN_W - 12, 17, GRAY);
    if (player_locked()) {
        Font *fb = F(800, 9);
        float bw = font_width(fb, "RADIO \xC2\xB7 NO SKIPS", 0) + 12, bx = SCREEN_W - 12 - 52 - bw;
        ui_rrect(bx, 10, bw, 14, 3, ACCENT);
        font_draw(fb, bx + 6, 20, "RADIO \xC2\xB7 NO SKIPS", HEX(0x000000, 255), 0);
    }
    font_draw(F(800, 10), 20, 50, "Now playing", GRAY, 0);
    draw_song_row(song, 20, 54, SCREEN_W - 40, 0, 0);
    font_draw(F(800, 10), 20, 100, player_locked() ? "Coming up on the radio" : "Next up", GRAY, 0);
    float top = 106, row = 30, bottom = SCREEN_H - 20;
    if (!queue_len) font_draw(F(500, 10), 20, top + 18, "Nothing after this song", GRAY, 0);
    queue_scroll = follow(queue_scroll, queue_sel, row, bottom - top, queue_len, dt);
    gfx_clip(0, top, SCREEN_W, bottom - top);
    int first = (int)(queue_scroll / row);
    for (int i = first; i < queue_len && i < first + 6; i++)
        draw_song_row(queue_buf[i], 20, top + i * row - queue_scroll, SCREEN_W - 40, i == queue_sel, 0);
    gfx_unclip();
    const int g[] = { ICON_CROSS, ICON_CIRCLE };
    const char *l[] = { player_locked() ? "No skips on radio" : "Play now", "Back" };
    draw_hints(SCREEN_W - 14, SCREEN_H - 6, g, l, 2, F(500, 9));
}

// Radio has no skips or seeking, so its now playing shows the station and
// the song instead of transport controls.
static void draw_now_radio(int song)
{
    const Song *s = &lib.songs[song];
    u32 c = darken(art_tint(song, s->album_id), 0.55f);
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    gfx_gradient(0, 0, SCREEN_W, SCREEN_H, c, color_lerp(c, BG, 0.55f), color_lerp(c, BG, 0.75f), BG);

    ui_shadow(18, 61 + 10, 150, 150, 6, 18, RGBA(0, 0, 0, 140));
    const Texture *t = art_get(song, ART_LARGE);
    if (t) gfx_tex(t, 18, 61, 150, 150, WHITE);
    else draw_art(song, ART_SMALL, 18, 61, 150, 6, s->album_id);

    float x = 186, w = SCREEN_W - 18 - x, right = SCREEN_W - 18;
    Font *f9 = F(500, 9), *fb = F(800, 9);
    ui_battery(f9, right, 23, GRAY);
    float bw = font_width(fb, "ON AIR", 0) + 12;
    ui_rrect(x, 16, bw, 14, 3, ACCENT);
    font_draw(fb, x + 6, 26, "ON AIR", HEX(0x000000, 255), 0);
    font_draw(f9, x + bw + 8, 27, "NO SKIPS", GRAY, 1);
    font_draw_fit(F(800, 22), x, 62, from_name, WHITE, w);

    gfx_rect(x, 80, w, 1, HEX(0xFFFFFF, 40));
    font_draw(f9, x, 104, player_paused() ? "PAUSED" : "NOW PLAYING", player_paused() ? GRAY : ACCENT, 1);
    // Level bars beside the label while music plays.
    if (!player_paused()) {
        float bx = x + font_width(f9, "NOW PLAYING", 1) + 10;
        for (int i = 0; i < 4; i++) {
            float h = 3 + 7 * (0.5f + 0.5f * sinf(t_now * (5 + i) + i * 1.7f));
            ui_rrect(bx + i * 5, 104 - h, 3, h, 1, ACCENT);
        }
    }
    font_draw_fit(F(800, 18), x, 128, song_title(s), WHITE, w);
    font_draw_fit(F(500, 11), x, 146, song_artist(s), LIGHT, w);
    font_draw_fit(F(500, 10), x, 162, song_album(s), GRAY, w);

    const int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE };
    const char *l[] = { player_paused() ? "Play" : "Pause", "Up next", "Back" };
    draw_hints(SCREEN_W - 18, SCREEN_H - 12, g, l, 3, F(500, 8));
}

static void draw_now(void)
{
    int song = player_song();
    if (song < 0) { pop(); return; }
    if (player_locked()) { draw_now_radio(song); return; }
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
    static const char *from_labels[] = { "PLAYING FROM ALBUM", "PLAYING FROM ARTIST", "PLAYING FROM SONGS",
                                         "PLAYING FROM PLAYLIST", "PLAYING FROM RADIO" };
    font_draw(f9, x, 27, from_labels[from_kind], GRAY, 1);
    float right = SCREEN_W - 18;
    ui_battery(f9, right, 23, GRAY);
    if (player_locked()) {
        Font *fb = F(800, 9);
        float bw = font_width(fb, "RADIO \xC2\xB7 NO SKIPS", 0) + 12, bx = right - 52 - bw;
        ui_rrect(bx, 16, bw, 14, 3, ACCENT);
        font_draw(fb, bx + 6, 26, "RADIO \xC2\xB7 NO SKIPS", HEX(0x000000, 255), 0);
    }
    font_draw_fit(F(700, 10), x, 44, from_kind == FROM_ALBUM ? song_album(s) : from_name, LIGHT, w);

    font_draw_fit(F(800, 20), x, 140, song_title(s), WHITE, w);
    font_draw_fit(F(500, 12), x, 158, song_artist(s), GRAY, w);

    // Progress.
    u32 pos = scrubbing ? (u32)scrub_ms : player_position_ms(), dur = s->duration_ms;
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

    if (player_locked()) {
        const int g[] = { ICON_CROSS, ICON_TRIANGLE, ICON_CIRCLE };
        const char *l[] = { player_paused() ? "Play" : "Pause", "Queue", "Back" };
        draw_hints(SCREEN_W - 18, SCREEN_H - 12, g, l, 3, F(500, 8));
    } else {
        const int g[] = { ICON_CROSS, -1, ICON_SQUARE, ICON_TRIANGLE, ICON_CIRCLE };
        const char *l[] = { player_paused() ? "Play" : "Pause", "L/R Skip", "Shuffle", "Queue", "Back" };
        draw_hints(SCREEN_W - 18, SCREEN_H - 12, g, l, 5, F(500, 8));
    }
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

// ---- Modals -------------------------------------------------------------------

static void draw_dim(void)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 178));
}

static void draw_add(void)
{
    draw_dim();
    float w = 280, row = 26, x = (SCREEN_W - w) / 2;
    int count = playlist_count + 1, shown = count < 6 ? count : 6;
    float h = 52 + shown * row + 22, y = (SCREEN_H - h) / 2;
    ui_shadow(x, y + 4, w, h, 10, 12, RGBA(0, 0, 0, 140));
    ui_rrect(x, y, w, h, 10, CARD_SEL);
    font_draw(F(800, 14), x + 14, y + 24, "Add to playlist", WHITE, 0);
    font_draw_fit(F(500, 10), x + 14, y + 40, song_title(&lib.songs[add_song]), GRAY, w - 28);
    int first = clampi(add_sel - shown + 1, 0, count - shown);
    for (int i = first; i < first + shown; i++) {
        float ry = y + 48 + (i - first) * row;
        if (i == add_sel) ui_rrect(x + 8, ry, w - 16, row - 2, 4, HEX(0x3A3A3A, 255));
        if (i == 0) {
            ui_icon(ICON_PLUS, x + 16, ry + 5, 14, ACCENT);
            font_draw(F(700, 11), x + 38, ry + 16, "New playlist", WHITE, 0);
        } else {
            const Playlist *pl = &playlists[i - 1];
            draw_playlist_cover(pl, x + 14, ry + 2, 20, 2);
            font_draw_fit(F(700, 11), x + 42, ry + 16, pl->name, WHITE, w - 100);
            char n[16];
            snprintf(n, sizeof(n), "%d", pl->count);
            font_draw_right(F(500, 9), x + w - 18, ry + 16, n, GRAY);
        }
    }
    const int g[] = { ICON_CROSS, ICON_CIRCLE };
    const char *l[] = { "Add", "Cancel" };
    draw_hints(x + w - 14, y + h - 9, g, l, 2, F(500, 9));
}

static void draw_menu(void)
{
    draw_dim();
    static const char *items[MENU_COUNT] = { "Rename", "Remove song", "Delete playlist" };
    const Playlist *pl = &playlists[page_playlist];
    float w = 240, row = 28, h = 46 + MENU_COUNT * row + 22, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    ui_shadow(x, y + 4, w, h, 10, 12, RGBA(0, 0, 0, 140));
    ui_rrect(x, y, w, h, 10, CARD_SEL);
    font_draw_fit(F(800, 14), x + 14, y + 26, pl->name, WHITE, w - 28);
    for (int i = 0; i < MENU_COUNT; i++) {
        float ry = y + 38 + i * row;
        int off = i == MENU_REMOVE && !pl->count;
        if (i == menu_sel) ui_rrect(x + 8, ry, w - 16, row - 2, 4, HEX(0x3A3A3A, 255));
        char label[200];
        if (i == MENU_REMOVE && pl->count)
            snprintf(label, sizeof(label), "Remove \"%s\"", song_title(&lib.songs[pl->songs[page_sel]]));
        else snprintf(label, sizeof(label), "%s", items[i]);
        u32 c = off ? DIM : i == MENU_DELETE ? HEX(0xFF5A5A, 255) : WHITE;
        font_draw_fit(F(700, 11), x + 16, ry + 17, label, c, w - 32);
    }
    const int g[] = { ICON_CROSS, ICON_CIRCLE };
    const char *l[] = { "Choose", "Close" };
    draw_hints(x + w - 14, y + h - 9, g, l, 2, F(500, 9));
}

static void draw_delete(void)
{
    draw_dim();
    const Playlist *pl = &playlists[page_playlist];
    float w = 260, h = 104, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    ui_rrect(x, y, w, h, 10, CARD_SEL);
    char title[64];
    snprintf(title, sizeof(title), "Delete \"%s\"?", pl->name);
    font_draw_fit(F(800, 14), x + 14, y + 26, title, WHITE, w - 28);
    font_draw(F(500, 10), x + 14, y + 44, "Your songs stay in the library. Only the", GRAY, 0);
    font_draw(F(500, 10), x + 14, y + 56, "playlist is removed.", GRAY, 0);
    ui_rrect(x + 14, y + 70, w - 28, 6, 3, HEX(0x404040, 255));
    float k = delete_t / DELETE_SECONDS;
    if (k > 0) ui_rrect(x + 14, y + 70, fmaxf(6, (w - 28) * k), 6, 3, HEX(0xFF5A5A, 255));
    float hx = font_draw(F(500, 9), x + 14, y + 94, "Hold ", LIGHT, 0);
    ui_hint(F(500, 9), hx, y + 94, ICON_SQUARE, "for 2 seconds to delete", LIGHT);
    ui_hint(F(500, 9), x + w - 14 - ui_hint_width(F(500, 9), ICON_CIRCLE, "Cancel"), y + 94, ICON_CIRCLE, "Cancel", LIGHT);
}

static void draw_keyboard(void)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, BG);
    font_draw(F(800, 14), 14, 24, kb_mode == KB_NEW ? "New playlist" : "Rename playlist", WHITE, 0);
    ui_battery(F(500, 9), SCREEN_W - 14, 18, GRAY);

    // Cover preview, name field with caret, character count.
    if (kb_song >= 0) draw_art(kb_song, ART_SMALL, 14, 36, 40, 2, lib.songs[kb_song].album_id);
    else if (kb_mode == KB_RENAME) draw_playlist_cover(&playlists[page_playlist], 14, 36, 40, 2);
    else { ui_rrect(14, 36, 40, 40, 2, CARD_SEL); ui_icon(ICON_NOTE, 24, 46, 20, GRAY); }
    Font *fn = F(800, 18);
    float fx = 66, fw = SCREEN_W - 14 - 40 - fx;
    float tw = font_width(fn, kb_text, 0), tx = fx + 2 - (tw > fw - 8 ? tw - (fw - 8) : 0);
    gfx_clip(fx, 36, fw, 40);
    float end = font_draw(fn, tx, 64, kb_text, WHITE, 0);
    gfx_unclip();
    if (fmodf(t_now, 1.0f) < 0.5f) gfx_rect(end + 2, 48, 2, 20, ACCENT);
    gfx_rect(fx, 74, fw, 2, ACCENT);
    char count[16];
    snprintf(count, sizeof(count), "%d/%d", (int)strlen(kb_text), PLAYLIST_NAME_MAX);
    font_draw_right(F(500, 9), SCREEN_W - 14, 62, count, GRAY);

    for (int r = 0; r < 4; r++) {
        Key keys[16];
        int n = kb_layout(r, keys);
        float y = 88 + r * 32;
        for (int i = 0; i < n; i++) {
            const Key *k = &keys[i];
            int on = r == kb_row && i == kb_col, special = k->kind == KEY_BACK || k->kind == KEY_SHIFT || k->kind == KEY_SPACE;
            u32 bg = on ? ACCENT : special ? HEX(0x1A1A1A, 255) : HEX(0x2A2A2A, 255), fg = on ? HEX(0x000000, 255) : WHITE;
            ui_rrect(k->x, y, k->w, 28, 5, bg);
            if (special && !on) ui_ring(k->x, y, k->w, 28, 5, HEX(0x3A3A3A, 255));
            float cx = k->x + k->w / 2;
            if (k->kind == KEY_BACK) ui_icon(ICON_BACKSPACE, cx - 8, y + 6, 16, fg);
            else if (k->kind == KEY_SHIFT) ui_icon(ICON_SHIFT, cx - 8, y + 6, 16, kb_caps && !on ? ACCENT : fg);
            else if (k->kind == KEY_SPACE) font_draw_center(F(700, 11), cx, y + 18, "space", fg);
            else {
                char c[2] = { k->ch >= 'A' && k->ch <= 'Z' && !kb_caps ? k->ch + 32 : k->ch, 0 };
                font_draw_center(F(700, 12), cx, y + 19, c, fg);
            }
        }
    }
    Font *fh = F(500, 9);
    const char *start = "START Save";
    float x = SCREEN_W - 14 - ui_hint_width(fh, ICON_CIRCLE, "Cancel");
    ui_hint(fh, x, SCREEN_H - 10, ICON_CIRCLE, "Cancel", GRAY);
    x -= 12 + font_width(F(800, 9), start, 0);
    font_draw(F(800, 9), x, SCREEN_H - 10, start, ACCENT, 0);
    x -= 12 + font_width(fh, "SELECT Caps", 0);
    font_draw(fh, x, SCREEN_H - 10, "SELECT Caps", GRAY, 0);
    x -= 12 + ui_hint_width(fh, ICON_SQUARE, "Delete");
    ui_hint(fh, x, SCREEN_H - 10, ICON_SQUARE, "Delete", GRAY);
    x -= 12 + ui_hint_width(fh, ICON_CROSS, "Type");
    ui_hint(fh, x, SCREEN_H - 10, ICON_CROSS, "Type", GRAY);
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
