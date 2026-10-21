// FL/AC: album and artist pages, now playing and the queue.
#include "flac_internal.h"

void update_queue(const Input *in)
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

void update_album_page(const Input *in)
{
    const Album *a = &lib.albums[page_album];
    u32 n = in->nav, p = in->pressed;
    if (n & PSP_CTRL_UP) page_sel = clampi(page_sel - 1, 0, a->count - 1);
    if (n & PSP_CTRL_DOWN) page_sel = clampi(page_sel + 1, 0, a->count - 1);
    if (p & PSP_CTRL_CROSS) play_album(page_album, page_sel, 0);
    if (p & PSP_CTRL_START) { play_album(page_album, -1, 1); push(VIEW_NOW); }
    if (p & PSP_CTRL_CIRCLE) pop();
}

void update_artist_page(const Input *in)
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

void update_now(const Input *in, float dt)
{
    u32 p = in->pressed;
    scrub_update(in, dt);
    if (p & PSP_CTRL_CROSS) player_toggle_pause();
    if (p & PSP_CTRL_RTRIGGER) player_next();
    if (p & PSP_CTRL_LTRIGGER) player_prev();
    if (p & PSP_CTRL_SQUARE) {
        player_set_shuffle(!player_shuffle());
        show_toast(player_shuffle() ? "Shuffle on" : "Shuffle off");
    }
    if (p & (PSP_CTRL_CIRCLE | PSP_CTRL_SELECT)) pop();
}

void draw_album_page(float dt)
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

void draw_artist_page(float dt)
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

void draw_queue(float dt)
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
    int kind;
    font_draw_fit(F(800, 22), x, 62, player_source(&kind), WHITE, w);

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

void draw_now(void)
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
    int from_kind;
    const char *from_name = player_source(&from_kind);
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
    u32 pos = scrub_position(), dur = s->duration_ms;
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
