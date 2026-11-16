#include "stats.h"
#include "glib.h"

#include <math.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "blib.h"
#include "library.h"
#include "player.h"
#include "ui.h"

// A play counts once a song is heard for 30 seconds, or half of a short one.
#define PLAY_MS 30000

typedef struct {
    u32 day, ms;
    s32 song;    // -1 when the file is gone from the library
    s16 station; // index into station_names, -1 when not radio
    s16 book;    // index into book_names for audiobook time, else -1
} Rec;

static Rec *recs;
static int rec_count, rec_cap;
static char (*station_names)[64];
static int station_name_count, station_name_cap;
static char (*book_names)[256];
static int book_name_count, book_name_cap;
static char log_path[300];
static int loaded;

// The song being listened to now.
static int cur_song = -1, cur_station = -1, cur_book = -1; // cur_book: into book_names
static float cur_ms;

// ---- Dates ---------------------------------------------------------------------

// Days since 1970-01-01 for a civil date, and back.
static u32 days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (u32)(era * 146097 + doe - 719468);
}

static void civil_from_days(u32 z, int *y, int *m, int *d)
{
    int zz = (int)z + 719468, era = zz / 146097, doe = zz - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yoe + era * 400 + (*m <= 2);
}

static u32 today(void)
{
    ScePspDateTime t;
    if (sceRtcGetCurrentClockLocalTime(&t) < 0) return 0;
    return days_from_civil(t.year, t.month, t.day);
}

// ---- Log ------------------------------------------------------------------------

static int station_index(const char *name)
{
    for (int i = 0; i < station_name_count; i++)
        if (!strcmp(station_names[i], name)) return i;
    if (station_name_count == station_name_cap) {
        station_name_cap = station_name_cap ? station_name_cap * 2 : 16;
        station_names = realloc(station_names, station_name_cap * sizeof(*station_names));
    }
    snprintf(station_names[station_name_count], 64, "%s", name);
    return station_name_count++;
}

static int book_index(const char *dir)
{
    for (int i = 0; i < book_name_count; i++)
        if (!strcmp(book_names[i], dir)) return i;
    if (book_name_count == book_name_cap) {
        book_name_cap = book_name_cap ? book_name_cap * 2 : 16;
        book_names = realloc(book_names, book_name_cap * sizeof(*book_names));
    }
    snprintf(book_names[book_name_count], 256, "%s", dir);
    return book_name_count++;
}

static void add_rec(u32 day, u32 ms, int song, int station, int book)
{
    if (rec_count == rec_cap) {
        rec_cap = rec_cap ? rec_cap * 2 : 256;
        recs = realloc(recs, rec_cap * sizeof(Rec));
    }
    recs[rec_count++] = (Rec){ day, ms, song, (s16)station, (s16)book };
}

static void add(u32 day, u32 ms, int song, int station) { add_rec(day, ms, song, station, -1); }

void stats_load(const char *data_dir)
{
    snprintf(log_path, sizeof(log_path), "%s/stats.log", data_dir);
    loaded = 1;
    SceUID fd = sceIoOpen(log_path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *buf = malloc(size + 1);
    int n = buf ? sceIoRead(fd, buf, size) : 0;
    sceIoClose(fd);
    if (!buf) return;
    buf[n > 0 ? n : 0] = 0;
    // "day<TAB>ms<TAB>station<TAB>path" per line; station is empty off the radio.
    for (char *line = buf, *next; *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        char *f[4];
        int k = 0;
        for (char *p = line; k < 4; k++) {
            f[k] = p;
            char *tab = k < 3 ? strchr(p, '\t') : NULL;
            if (k < 3 && !tab) break;
            if (tab) { *tab = 0; p = tab + 1; }
        }
        if (k < 4) continue;
        // Audiobook time: "book:<book folder>" in place of the song.
        if (!strncmp(f[3], "book:", 5)) add_rec(strtoul(f[0], NULL, 10), strtoul(f[1], NULL, 10), -1, -1, book_index(f[3] + 5));
        else add(strtoul(f[0], NULL, 10), strtoul(f[1], NULL, 10), library_find_path(f[3]), f[2][0] ? station_index(f[2]) : -1);
    }
    free(buf);
}

void stats_flush(void)
{
    if ((cur_song < 0 && cur_book < 0) || cur_ms < 1000 || !loaded) { cur_ms = 0; return; }
    u32 day = today(), ms = (u32)cur_ms;
    char line[512];
    int n;
    if (cur_song < 0) {
        add_rec(day, ms, -1, -1, cur_book);
        n = snprintf(line, sizeof(line), "%u\t%u\t\tbook:%s\n", (unsigned)day, (unsigned)ms, book_names[cur_book]);
    } else {
        add(day, ms, cur_song, cur_station);
        n = snprintf(line, sizeof(line), "%u\t%u\t%s\t%s\n", (unsigned)day, (unsigned)ms,
                     cur_station >= 0 ? station_names[cur_station] : "", song_path(&lib.songs[cur_song]));
    }
    SceUID fd = sceIoOpen(log_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
        sceIoClose(fd);
    }
    cur_ms = 0;
}

void stats_frame(float dt)
{
    if (!loaded) return;
    int song = player_song();
    int b = song < 0 && player_book_track() >= 0 ? blib_playing() : -1;
    int book = b >= 0 && b < book_count ? (cur_book >= 0 && !strcmp(book_names[cur_book], books[b].dir) ? cur_book : book_index(books[b].dir)) : -1;
    if (song != cur_song || book != cur_book) {
        stats_flush();
        cur_song = song;
        cur_book = book;
        cur_station = -1;
        if (song >= 0 && player_locked()) {
            int kind;
            cur_station = station_index(player_source(&kind));
        }
    }
    // A minute's worth logged now and then, so a long chapter isn't lost.
    if ((song >= 0 || book >= 0) && !player_paused()) cur_ms += dt * 1000;
    if (book >= 0 && cur_ms >= 60000) stats_flush();
}

// ---- Summing up ---------------------------------------------------------------

void stats_format_time(char *out, int size, u32 ms)
{
    u32 m = ms / 60000;
    if (m < 60) snprintf(out, size, "%um", (unsigned)m);
    else if (m < 6000) snprintf(out, size, "%uh %02um", (unsigned)(m / 60), (unsigned)(m % 60));
    else snprintf(out, size, "%uh", (unsigned)(m / 60));
}

static int is_play(const Rec *r)
{
    if (r->song < 0) return 0;
    u32 dur = lib.songs[r->song].duration_ms;
    return r->ms >= PLAY_MS || (dur && r->ms * 2 >= dur);
}

// Keeps the TOP_MAX highest counts, ids into out.
static int top_n(const u16 *counts, int n, int *out, int *out_counts)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!counts[i]) continue;
        int at = k < TOP_MAX ? k : TOP_MAX;
        while (at > 0 && out_counts[at - 1] < counts[i]) at--;
        if (at >= TOP_MAX) continue;
        int last = k < TOP_MAX ? k : TOP_MAX - 1;
        memmove(out + at + 1, out + at, (last - at) * sizeof(int));
        memmove(out_counts + at + 1, out_counts + at, (last - at) * sizeof(int));
        out[at] = i;
        out_counts[at] = counts[i];
        if (k < TOP_MAX) k++;
    }
    return k;
}

void stats_compute(int period, Stats *s)
{
    memset(s, 0, sizeof(*s));
    s->prev_plays = -1;
    u32 now = today();
    // The song in progress counts too, so the numbers move while it plays.
    int live = (cur_song >= 0 || cur_book >= 0) && cur_ms >= 1000;
    if (live) add_rec(now, (u32)cur_ms, cur_song, cur_station, cur_song >= 0 ? -1 : cur_book);

    u32 span = period == PERIOD_WEEK ? 7 : period == PERIOD_MONTH ? 28 : 0;
    u32 first = span && now >= span - 1 ? now - (span - 1) : 0;
    int n_songs = lib.song_count;
    u16 *counts = calloc(n_songs + lib.artist_count + lib.album_count + 3, sizeof(u16));
    u16 *artist_counts = counts + n_songs + 1, *album_counts = artist_counts + lib.artist_count + 1;
    u32 *station_ms = calloc(station_name_count + 1, sizeof(u32));
    u32 *book_ms = calloc(book_name_count + 1, sizeof(u32));

    // Bars: days of the week, weeks of the month, or the last 7 months.
    static const char *wd = "TFSSMTW"; // 1970-01-01 was a Thursday
    static const char *mo = "JFMAMJJASOND";
    int ny = 0, nm = 0, nd = 0;
    civil_from_days(now, &ny, &nm, &nd);
    if (period == PERIOD_WEEK) {
        s->bar_count = 7;
        for (int i = 0; i < 7; i++) s->bar_labels[i][0] = wd[(now - 6 + i) % 7];
        s->today_bar = 6;
    } else if (period == PERIOD_MONTH) {
        s->bar_count = 4;
        for (int i = 0; i < 4; i++) snprintf(s->bar_labels[i], 4, "W%d", i + 1);
        s->today_bar = 3;
    } else {
        s->bar_count = 7;
        for (int i = 0; i < 7; i++) s->bar_labels[i][0] = mo[((nm - 7 + i) % 12 + 12) % 12];
        s->today_bar = 6;
    }

    for (int i = 0; i < rec_count; i++) {
        const Rec *r = &recs[i];
        if (span && period != PERIOD_ALL && r->day + 2 * span > now && r->day < first && is_play(r) && !lib.songs[r->song].radio)
            s->prev_plays++;
        if (r->day < first || r->day > now) continue;
        if (r->book >= 0) { s->book_ms += r->ms; book_ms[r->book] += r->ms; }
        else s->ms += r->ms;
        if (r->station >= 0) station_ms[r->station] += r->ms;
        if (period == PERIOD_WEEK) s->bars[r->day - first] += r->ms;
        else if (period == PERIOD_MONTH) s->bars[(r->day - first) / 7] += r->ms;
        else {
            int y, m, d;
            civil_from_days(r->day, &y, &m, &d);
            int back = (ny - y) * 12 + (nm - m);
            if (back >= 0 && back < 7) s->bars[6 - back] += r->ms;
        }
        if (!is_play(r) || lib.songs[r->song].radio) continue;
        s->plays++;
        const Song *song = &lib.songs[r->song];
        counts[r->song]++;
        if (song->artist_id >= 0) artist_counts[song->artist_id]++;
        if (song->album_id >= 0) album_counts[song->album_id]++;
    }
    if (span && s->prev_plays >= 0) s->prev_plays++; // started at -1
    s->top_count[TOP_SONGS] = top_n(counts, n_songs, s->top[TOP_SONGS], s->top_plays[TOP_SONGS]);
    s->top_count[TOP_ARTISTS] = top_n(artist_counts, lib.artist_count, s->top[TOP_ARTISTS], s->top_plays[TOP_ARTISTS]);
    s->top_count[TOP_ALBUMS] = top_n(album_counts, lib.album_count, s->top[TOP_ALBUMS], s->top_plays[TOP_ALBUMS]);
    // Top books by time listened.
    for (int i = 0; i < book_name_count; i++) {
        if (!book_ms[i]) continue;
        int at = s->top_book_count;
        while (at > 0 && s->top_book_ms[at - 1] < book_ms[i]) at--;
        if (at >= TOP_MAX) continue;
        int last = s->top_book_count < TOP_MAX ? s->top_book_count : TOP_MAX - 1;
        memmove(s->top_book + at + 1, s->top_book + at, (last - at) * sizeof(int));
        memmove(s->top_book_ms + at + 1, s->top_book_ms + at, (last - at) * sizeof(u32));
        s->top_book[at] = i;
        s->top_book_ms[at] = book_ms[i];
        if (s->top_book_count < TOP_MAX) s->top_book_count++;
    }
    free(book_ms);
    for (int i = 0; i < station_name_count; i++)
        if (station_ms[i] > s->station_ms) {
            s->station_ms = station_ms[i];
            snprintf(s->station, sizeof(s->station), "%s", station_names[i]);
        }

    // Streak: days in a row with listening, ending today (or yesterday, so a
    // streak isn't lost before today's first song).
    u8 *heard = calloc(400, 1);
    for (int i = 0; i < rec_count; i++)
        if (recs[i].day <= now && now - recs[i].day < 400) heard[now - recs[i].day] = 1;
    int start = heard[0] ? 0 : 1;
    while (start + s->streak < 400 && heard[start + s->streak]) s->streak++;
    free(heard);
    free(counts);
    free(station_ms);
    if (live) rec_count--;
}

// ---- Screen ---------------------------------------------------------------------

#define BG HEX(0x0B0C10, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define LABEL HEX(0xA9ACB8, 255)
#define MUTED HEX(0x8A8D99, 255)
#define ACCENT HEX(0x6C8CFF, 255)
#define CARD RGBA(255, 255, 255, 13)
#define EDGE RGBA(255, 255, 255, 20)

static int period, top_kind;
static Stats st;
static float t_now, refresh_t, grow;
static Texture *t_glow;

static Font *S(int weight, int px) { return ui_font(FAMILY_SORA, weight, px); }

void stats_enter(void)
{
    period = PERIOD_WEEK;
    top_kind = TOP_SONGS;
    stats_compute(period, &st);
    refresh_t = 0;
    grow = 0;
}

int stats_update(const Input *in, float dt)
{
    t_now += dt;
    grow = fminf(1, grow + dt / 0.6f);
    u32 p = in->pressed;
    int changed = 0;
    if (p & PSP_CTRL_LTRIGGER) { period = (period + PERIOD_COUNT - 1) % PERIOD_COUNT; changed = 1; }
    if (p & PSP_CTRL_RTRIGGER) { period = (period + 1) % PERIOD_COUNT; changed = 1; }
    // The top list cycles tracks, artists, albums, games and then books.
    if (p & PSP_CTRL_DOWN) top_kind = (top_kind + 1) % (TOP_KINDS + 2);
    if (p & PSP_CTRL_UP) top_kind = (top_kind + TOP_KINDS + 1) % (TOP_KINDS + 2);
    if ((p & PSP_CTRL_SQUARE) && player_song() >= 0) player_toggle_pause();
    refresh_t += dt;
    if (changed) grow = 0;
    if (changed || refresh_t > 5) {
        refresh_t = 0;
        stats_compute(period, &st);
    }
    return (p & PSP_CTRL_CIRCLE) != 0;
}

static void card(float x, float y, float w, float h)
{
    ui_rrect(x, y, w, h, 8, CARD);
    ui_ring(x, y, w, h, 8, EDGE);
}

static void key_cap(Font *f, float x, float cy, const char *k)
{
    float w = font_width(f, k, 0) + 8;
    ui_ring(x, cy - 6, w, 12, 3, HEX(0x3A3D48, 255));
    font_draw(f, x + 4, cy + 3, k, MUTED, 0);
}

static void draw_top_row(int kind, int i, float x, float y, float w)
{
    int id = st.top[kind][i], seed = id;
    const char *title = "", *sub = "";
    int art_song = -1;
    if (kind == TOP_SONGS) {
        const Song *s = &lib.songs[id];
        title = song_title(s);
        sub = song_artist(s);
        art_song = id;
        seed = s->album_id;
    } else if (kind == TOP_ARTISTS) {
        title = lib.artists[id].name;
        art_song = lib.artists[id].art_song;
    } else {
        title = lib.albums[id].name;
        sub = lib.albums[id].artist;
        art_song = lib.albums[id].art_song;
    }
    (void)sub;
    Font *fn = S(700, 9);
    char num[4];
    snprintf(num, sizeof(num), "%d", i + 1);
    font_draw(fn, x, y + 14, num, i == 0 ? ACCENT : MUTED, 0);
    const Texture *t = art_song >= 0 ? art_get(art_song, kind == TOP_ARTISTS ? ART_ROUND : ART_SMALL) : NULL;
    if (t) gfx_tex(t, x + 12, y + 2, 20, 20, 0xFFFFFFFF);
    else ui_placeholder_art(x + 12, y + 2, 20, kind == TOP_ARTISTS ? 10 : 3, seed);
    char plays[12];
    snprintf(plays, sizeof(plays), "%d", st.top_plays[kind][i]);
    Font *fp = S(400, 9);
    float pw = font_width(fp, plays, 0), tx = x + 38, tw = w - 38 - pw - 8;
    font_draw_fit(S(700, 9), tx, y + 10, title, TEXT, tw);
    ui_rrect(tx, y + 15, tw, 3, 1.5f, RGBA(255, 255, 255, 26));
    float k = (float)st.top_plays[kind][i] / st.top_plays[kind][0];
    ui_rrect(tx, y + 15, fmaxf(3, tw * k * grow), 3, 1.5f, ACCENT);
    font_draw(fp, x + w - pw, y + 14, plays, HEX(0xC9CBD4, 255), 0);
}

void stats_draw(float dt)
{
    (void)dt;
    if (!t_glow) t_glow = tex_glow(128);
    gfx_begin(BG);
    float sa = 1 + 0.15f * (0.5f + 0.5f * sinf(t_now * 0.45f));
    float ax = 90 + 20 * sinf(t_now * 0.45f), ay = 50 + 9 * sinf(t_now * 0.45f);
    gfx_tex(t_glow, ax - 150 * sa, ay - 130 * sa, 300 * sa, 260 * sa, color_alpha(ACCENT, 0.25f));

    // Header: title, period tabs between L and R, battery.
    Font *f8 = S(400, 8), *f10 = S(700, 10);
    font_draw(S(800, 15), 14, 25, "Stats", TEXT, 0);
    float x = 70;
    key_cap(f8, x, 20, "L");
    x += 20;
    static const char *names[PERIOD_COUNT] = { "Week", "Month", "All time" };
    for (int i = 0; i < PERIOD_COUNT; i++) {
        float w = font_width(f10, names[i], 0) + 16;
        if (i == period) ui_rrect(x, 12, w, 16, 8, ACCENT);
        font_draw(f10, x + 8, 24, names[i], i == period ? BG : HEX(0xB5B8C4, 255), 0);
        x += w + 4;
    }
    key_cap(f8, x + 2, 20, "R");
    ui_battery(S(400, 10), SCREEN_W - 14, 20, HEX(0xB5B8C4, 255));

    // Key numbers.
    char v[4][48], note[4][48];
    stats_format_time(v[0], sizeof(v[0]), st.ms + st.book_ms);
    if (st.book_ms) {
        char bt[24];
        stats_format_time(bt, sizeof(bt), st.book_ms);
        snprintf(note[0], sizeof(note[0]), "Books %s, radio too", bt);
    } else snprintf(note[0], sizeof(note[0]), "Includes radio");
    snprintf(v[1], sizeof(v[1]), "%d", st.plays);
    if (st.prev_plays > 0) {
        int pct = (st.plays - st.prev_plays) * 100 / st.prev_plays;
        snprintf(note[1], sizeof(note[1]), "%+d%% vs last %s", pct, period == PERIOD_WEEK ? "week" : "month");
    } else snprintf(note[1], sizeof(note[1]), "%s", period == PERIOD_ALL ? "Since day one" : "First one");
    snprintf(v[2], sizeof(v[2]), "%d day%s", st.streak, st.streak == 1 ? "" : "s");
    snprintf(note[2], sizeof(note[2]), "%s", st.streak ? "Keep it going" : "Play a song today");
    if (st.top_count[TOP_ARTISTS]) {
        snprintf(v[3], sizeof(v[3]), "%s", lib.artists[st.top[TOP_ARTISTS][0]].name);
        snprintf(note[3], sizeof(note[3]), "%d plays", st.top_plays[TOP_ARTISTS][0]);
    } else {
        snprintf(v[3], sizeof(v[3]), "\xE2\x80\x94");
        snprintf(note[3], sizeof(note[3]), "No plays yet");
    }
    static const char *labels[4] = { "LISTENED", "PLAYS", "STREAK", "TOP ARTIST" };
    float kw = (SCREEN_W - 28 - 24) / 4.0f;
    for (int i = 0; i < 4; i++) {
        float kx = 14 + i * (kw + 8), ky = 38;
        card(kx, ky, kw, 44);
        font_draw(f8, kx + 8, ky + 12, labels[i], LABEL, 0.5f);
        font_draw_fit(S(700, 15), kx + 8, ky + 30, v[i], TEXT, kw - 16);
        font_draw_fit(f8, kx + 8, ky + 40, note[i], i == 1 && st.prev_plays > 0 ? ACCENT : MUTED, kw - 16);
    }

    // Listening per day (or week, or month).
    float lx = 14, lw = (SCREEN_W - 28 - 10) / 2.0f, ly = 90, lh = 110;
    card(lx, ly, lw, lh);
    static const char *bar_titles[PERIOD_COUNT] = { "Listening per day", "Listening per week", "Listening per month" };
    font_draw(S(400, 9), lx + 8, ly + 13, bar_titles[period], LABEL, 0);
    u32 max = 1;
    for (int i = 0; i < st.bar_count; i++) if (st.bars[i] > max) max = st.bars[i];
    float bx = lx + 8, bw = (lw - 16 - (st.bar_count - 1) * 6) / st.bar_count, base = ly + lh - 16;
    for (int i = 0; i < st.bar_count; i++) {
        float h = st.bars[i] ? fmaxf(2, 66.0f * st.bars[i] / max) : 2;
        h *= grow;
        u32 c = i == st.today_bar ? ACCENT : RGBA(255, 255, 255, 56);
        gfx_rect(bx + i * (bw + 6), base - h, bw, h, c);
        font_draw_center(S(400, 7), bx + i * (bw + 6) + bw / 2, base + 10, st.bar_labels[i], MUTED);
    }

    // Top radio station.
    float sy = ly + lh + 6;
    card(lx, sy, lw, 32);
    float rx = lx + 9, rcy = sy + 16;
    ui_ring(rx + 4, rcy - 2, 4, 4, 2, ACCENT);
    ui_ring(rx, rcy - 6, 12, 12, 6, color_alpha(ACCENT, 0.7f));
    font_draw(f8, lx + 28, sy + 13, "Top radio station", LABEL, 0);
    char station[96], sm[24] = "";
    if (st.station[0]) {
        snprintf(station, sizeof(station), "%s", st.station);
        stats_format_time(sm, sizeof(sm), st.station_ms);
    } else snprintf(station, sizeof(station), "No radio yet");
    Font *fs = S(700, 11);
    float smw = font_width(fs, sm, 0);
    font_draw_fit(S(700, 10), lx + 28, sy + 26, station, TEXT, lw - 40 - smw);
    if (sm[0]) font_draw_right(fs, lx + lw - 9, sy + 21, sm, TEXT);

    // Top songs, artists or albums.
    float tx = lx + lw + 10, ty = ly, th = lh + 38;
    card(tx, ty, lw, th);
    static const char *top_titles[TOP_KINDS + 2] = { "Top tracks", "Top artists", "Top albums", "Top games", "Top books" };
    font_draw(S(400, 9), tx + 8, ty + 13, top_titles[top_kind], LABEL, 0);
    font_draw_right(S(400, 9), tx + lw - 8, ty + 13, top_kind == TOP_KINDS ? "Played" : top_kind > TOP_KINDS ? "Listened" : "Plays", LABEL);
    if (top_kind == TOP_KINDS + 1) {
        if (!st.top_book_count) font_draw(S(400, 10), tx + 8, ty + 40, "No books heard yet", MUTED, 0);
        for (int i = 0; i < st.top_book_count; i++) {
            float ry = ty + 20 + i * 25, rx = tx + 8, rw = lw - 16;
            const char *dir = book_names[st.top_book[i]], *title = strrchr(dir, '/');
            title = title ? title + 1 : dir;
            int bi = -1;
            for (int j = 0; j < book_count; j++) if (!strcmp(books[j].dir, dir)) { bi = j; title = books[j].title; break; }
            char tm[24];
            stats_format_time(tm, sizeof(tm), st.top_book_ms[i]);
            font_draw(S(700, 9), rx, ry + 15, (char[2]){ (char)('1' + i), 0 }, i == 0 ? ACCENT : MUTED, 0);
            const Texture *c = bi >= 0 && books[bi].cover[0] ? art_get_file(books[bi].cover, ART_COVER_S) : NULL;
            if (c) {
                float h = 22, w = h * c->w / (float)c->h;
                if (w > 22) w = 22;
                gfx_filter(1);
                gfx_tex(c, rx + 12 + (22 - w) / 2, ry + 1, w, h, 0xFFFFFFFF);
            } else ui_placeholder_art(rx + 12, ry + 2, 20, 3, i + 21);
            Font *ft = S(700, 9);
            float tw = font_width(ft, tm, 0);
            font_draw_fit(S(600, 10), rx + 40, ry + 16, title, TEXT, rw - 46 - tw);
            font_draw(ft, rx + rw - tw, ry + 16, tm, HEX(0xC9CBD4, 255), 0);
        }
    } else if (top_kind == TOP_KINDS) {
        GameStats gs;
        glib_stats(period == PERIOD_WEEK ? 7 : period == PERIOD_MONTH ? 28 : 0, &gs);
        if (!gs.top_count) font_draw(S(400, 10), tx + 8, ty + 40, "No games played yet", MUTED, 0);
        for (int i = 0; i < gs.top_count; i++) {
            float ry = ty + 20 + i * 25, rx = tx + 8, rw = lw - 16;
            char tm[24];
            glib_format_time(tm, sizeof(tm), gs.top_secs[i]);
            font_draw(S(700, 9), rx, ry + 15, (char[2]){ (char)('1' + i), 0 }, MUTED, 0);
            const Texture *icon = glib_icon(gs.top_game[i]);
            if (icon) { gfx_filter(1); gfx_tex(icon, rx + 12, ry + 3, 36, 20, 0xFFFFFFFF); }
            else ui_placeholder_art(rx + 12, ry + 3, 20, 3, i + 11);
            Font *ft = S(700, 9);
            float tw = font_width(ft, tm, 0);
            font_draw_fit(S(600, 10), rx + 54, ry + 16, gs.top_title[i], TEXT, rw - 60 - tw);
            font_draw(ft, rx + rw - tw, ry + 16, tm, HEX(0xC9CBD4, 255), 0);
        }
    } else {
        if (!st.top_count[top_kind]) font_draw(S(400, 10), tx + 8, ty + 40, "Nothing played yet", MUTED, 0);
        for (int i = 0; i < st.top_count[top_kind]; i++) draw_top_row(top_kind, i, tx + 8, ty + 20 + i * 25, lw - 16);
    }

    // Hints.
    static const char *next_names[TOP_KINDS + 2] = { "Artists", "Albums", "Games", "Books", "Tracks" };
    char down[32];
    snprintf(down, sizeof(down), "Down: %s", next_names[top_kind]);
    Font *fh = S(400, 9);
    float hx = SCREEN_W - 14 - ui_hint_width(fh, ICON_CIRCLE, "Back");
    ui_hint(fh, hx, SCREEN_H - 8, ICON_CIRCLE, "Back", LABEL);
    hx -= 12 + font_width(fh, down, 0);
    font_draw(fh, hx, SCREEN_H - 8, down, LABEL, 0);
    hx -= 12 + font_width(fh, "L/R Period", 0);
    font_draw(fh, hx, SCREEN_H - 8, "L/R Period", LABEL, 0);
    ui_volume(S(700, 10), HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
