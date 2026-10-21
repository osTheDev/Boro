// FL/AC: the home section with its rotating suggestions, and the album,
// artist and song lists.
#include "flac_internal.h"

static Slide slides[4];
static u32 tint, tint_target;
static int jump[4], jump_count;
static float album_scroll, artist_scroll, song_scroll;

void build_slides(void)
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

void update_section(const Input *in)
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

static const char *greeting(void)
{
    char clock[16];
    sys_clock(clock, sizeof(clock));
    int h = atoi(clock) % 12, pm = strstr(clock, "PM") != NULL;
    int hour = h + (pm ? 12 : 0);
    return hour < 5 ? "Good night" : hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening";
}

void draw_home(float dt)
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

void draw_albums(float dt)
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

void draw_artists(float dt)
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

void draw_song_row(int song, float x, float y, float w, int selected, int number)
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

void draw_songs(float dt)
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

void draw_scanning(void)
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
