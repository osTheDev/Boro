// FL/AC: playlists, radio stations, the artist picker for new stations, the
// playlist keyboard and the add / rename / delete modals.
#include "flac_internal.h"

static int remove_armed = -1;
static float delete_t;

// Triangle tapped on a station: press again to remove it.
void tap_station(void)
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

void update_playlist_page(const Input *in)
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

void update_pick_artist(const Input *in)
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

void update_keyboard(const Input *in)
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

void update_add(const Input *in)
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

void update_menu(const Input *in)
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

void update_delete(const Input *in, float dt)
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

void draw_playlists(float dt)
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

void draw_radio(float dt)
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

void draw_playlist_page(float dt)
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

void draw_pick_artist(float dt)
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

static void draw_dim(void)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 178));
}

void draw_add(void)
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

void draw_menu(void)
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

void draw_delete(void)
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

void draw_keyboard(void)
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
