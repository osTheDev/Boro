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
#include "skin.h"

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
static GameStats gs;
static float t_now, refresh_t, grow;

static Font *S(int weight, int px) { return ui_font(FAMILY_SORA, weight, px); }

static void refresh_games(void) { glib_stats(period == PERIOD_WEEK ? 7 : period == PERIOD_MONTH ? 28 : 0, &gs); }

void stats_enter(void)
{
    period = PERIOD_WEEK;
    top_kind = TOP_SONGS;
    stats_compute(period, &st);
    refresh_games();
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
        refresh_games();
    }
    return (p & PSP_CTRL_CIRCLE) != 0;
}

// ---- Drawing: the Boro skin (Stream or Web, as Settings > Home style) -------


static const char *const kind_names[TOP_KINDS + 2] = { "Tracks", "Artists", "Albums", "Games", "Books" };

// One entry of the top list: its title, a second line, the number on the
// right and how full its bar is.
typedef struct {
    const char *title, *sub;
    char value[24];
    float frac;
    const Texture *art;
    int art_wide; // a game icon, wider than tall
} Top;

static int top_count(int kind)
{
    if (kind == TOP_KINDS) return gs.top_count;
    if (kind == TOP_KINDS + 1) return st.top_book_count;
    return st.top_count[kind];
}

static void top_item(int kind, int i, Top *t)
{
    memset(t, 0, sizeof(*t));
    t->sub = "";
    if (kind == TOP_KINDS) {
        t->title = gs.top_title[i];
        glib_format_time(t->value, sizeof(t->value), gs.top_secs[i]);
        t->frac = gs.top_secs[0] ? (float)gs.top_secs[i] / gs.top_secs[0] : 0;
        t->art = glib_icon(gs.top_game[i]);
        t->art_wide = 1;
    } else if (kind == TOP_KINDS + 1) {
        const char *dir = book_names[st.top_book[i]], *slash = strrchr(dir, '/');
        t->title = slash ? slash + 1 : dir;
        for (int j = 0; j < book_count; j++)
            if (!strcmp(books[j].dir, dir)) {
                t->title = books[j].title;
                t->sub = books[j].author;
                if (books[j].cover[0]) t->art = art_get_file(books[j].cover, ART_COVER_S);
                break;
            }
        stats_format_time(t->value, sizeof(t->value), st.top_book_ms[i]);
        t->frac = st.top_book_ms[0] ? (float)st.top_book_ms[i] / st.top_book_ms[0] : 0;
    } else {
        int id = st.top[kind][i], art_song = -1;
        if (kind == TOP_SONGS) {
            t->title = song_title(&lib.songs[id]);
            t->sub = song_artist(&lib.songs[id]);
            art_song = id;
        } else if (kind == TOP_ARTISTS) {
            t->title = lib.artists[id].name;
            art_song = lib.artists[id].art_song;
        } else {
            t->title = lib.albums[id].name;
            t->sub = lib.albums[id].artist;
            art_song = lib.albums[id].art_song;
        }
        if (art_song >= 0) t->art = art_get(art_song, kind == TOP_ARTISTS ? ART_ROUND : ART_SMALL);
        snprintf(t->value, sizeof(t->value), "%d", st.top_plays[kind][i]);
        t->frac = (float)st.top_plays[kind][i] / st.top_plays[kind][0];
    }
}

static void draw_art(const Top *t, float x, float y, float size, int seed)
{
    gfx_filter(1);
    if (!t->art) { ui_placeholder_art(x, y, size, 3, seed); return; }
    float w = size, h = size;
    if (t->art_wide) w = size * 1.8f;
    else if (t->art->w != t->art->h) w = size * t->art->w / (float)t->art->h;
    gfx_tex(t->art, x, y, w, h, 0xFFFFFFFF);
}

static const char *period_label(void)
{
    return period == PERIOD_WEEK ? "THIS WEEK" : period == PERIOD_MONTH ? "THE LAST 4 WEEKS" : "ALL TIME";
}

static void key_numbers(char v[4][48], char note[4][48])
{
    snprintf(v[0], 48, "%d", st.plays);
    if (st.prev_plays > 0) {
        int pct = (st.plays - st.prev_plays) * 100 / st.prev_plays;
        snprintf(note[0], 48, "%+d%% vs last %s", pct, period == PERIOD_WEEK ? "week" : "month");
    } else snprintf(note[0], 48, "%s", period == PERIOD_ALL ? "Since day one" : "First one");
    snprintf(v[1], 48, "%d day%s", st.streak, st.streak == 1 ? "" : "s");
    snprintf(note[1], 48, "%s", st.streak ? "Keep it going" : "Play a song today");
    if (st.top_count[TOP_ARTISTS]) {
        snprintf(v[2], 48, "%s", lib.artists[st.top[TOP_ARTISTS][0]].name);
        snprintf(note[2], 48, "%d plays", st.top_plays[TOP_ARTISTS][0]);
    } else {
        snprintf(v[2], 48, "\xE2\x80\x94");
        snprintf(note[2], 48, "No plays yet");
    }
    if (st.station[0]) {
        snprintf(v[3], 48, "%s", st.station);
        stats_format_time(note[3], 48, st.station_ms);
    } else {
        snprintf(v[3], 48, "\xE2\x80\x94");
        snprintf(note[3], 48, "No radio yet");
    }
}

static void stream_draw(void)
{
    skin_stream_back(HEX(0x8A6A3C, 255));
    skin_stream_header("Stats");
    Font *fp = S(600, 9);
    float x = 150;
    static const char *names[PERIOD_COUNT] = { "Week", "Month", "All time" };
    for (int i = 0; i < PERIOD_COUNT; i++) x += skin_pill(fp, x, 12, names[i], i == period) + 4;

    // Hero: everything listened and played in the period.
    char total[24], music[24], games[24], books_t[24], sub[160], label[64];
    stats_format_time(total, sizeof(total), st.ms + st.book_ms + gs.seconds * 1000);
    stats_format_time(music, sizeof(music), st.ms);
    stats_format_time(games, sizeof(games), gs.seconds * 1000);
    stats_format_time(books_t, sizeof(books_t), st.book_ms);
    snprintf(label, sizeof(label), "%s \xC2\xB7 LISTENED AND PLAYED", period_label());
    font_draw(S(400, 9), 16, 50, label, ACCENT, 1);
    font_draw(S(700, 26), 16, 78, total, TEXT, 0);
    snprintf(sub, sizeof(sub), "Music %s \xC2\xB7 Games %s \xC2\xB7 Books %s \xC2\xB7 %d-day streak", music, games, books_t, st.streak);
    font_draw_fit(S(400, 10), 16, 94, sub, MUTED, 280);

    // Listening per day (week, month) beside it.
    u32 max = 1;
    for (int i = 0; i < st.bar_count; i++) if (st.bars[i] > max) max = st.bars[i];
    float span = 160, bw = 12, gap = (span - st.bar_count * bw) / (st.bar_count > 1 ? st.bar_count - 1 : 1), bx = SCREEN_W - 16 - span;
    for (int i = 0; i < st.bar_count; i++) {
        float h = (st.bars[i] ? fmaxf(3, 54.0f * st.bars[i] / max) : 3) * grow, xx = bx + i * (bw + gap);
        ui_rrect(xx, 92 - h, bw, h, 3, i == st.today_bar ? ACCENT : HEX(0x5A6078, 255));
        font_draw_center(S(400, 7), xx + bw / 2, 104, st.bar_labels[i], MUTED);
    }

    // Key numbers, as cards like the home tiles.
    char v[4][48], note[4][48];
    key_numbers(v, note);
    static const char *labels[4] = { "PLAYS", "STREAK", "TOP ARTIST", "TOP STATION" };
    float cw = (SCREEN_W - 32 - 3 * 8) / 4.0f;
    for (int i = 0; i < 4; i++) {
        float cx = 16 + i * (cw + 8), cy = 114;
        skin_glass(cx, cy, cw, 44, 0);
        font_draw(S(400, 8), cx + 8, cy + 12, labels[i], MUTED, 0.5f);
        font_draw_fit(S(700, 13), cx + 8, cy + 28, v[i], TEXT, cw - 16);
        font_draw_fit(S(400, 8), cx + 8, cy + 39, note[i], i == 0 && st.prev_plays > 0 ? ACCENT : MUTED, cw - 16);
    }

    // The top list, with its kinds as chips.
    float ty = 164, tw = SCREEN_W - 32;
    skin_glass(16, ty, tw, 80, 0);
    char head[32];
    snprintf(head, sizeof(head), "Top %s", kind_names[top_kind]);
    for (char *c = head + 4; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    font_draw(S(700, 11), 24, ty + 15, head, TEXT, 0);
    x = 112;
    for (int i = 0; i < TOP_KINDS + 2; i++) x += skin_pill(fp, x, ty + 5, kind_names[i], i == top_kind) + 4;
    int n = top_count(top_kind);
    if (!n) font_draw(S(400, 10), 24, ty + 46, top_kind == TOP_KINDS ? "No games played yet" : top_kind > TOP_KINDS ? "No books heard yet" : "Nothing played yet", MUTED, 0);
    for (int i = 0; i < n && i < 3; i++) {
        Top t;
        top_item(top_kind, i, &t);
        float ry = ty + 26 + i * 17;
        font_draw(S(700, 9), 24, ry + 11, (char[2]){ (char)('1' + i), 0 }, i == 0 ? ACCENT : MUTED, 0);
        draw_art(&t, 36, ry, 14, i + top_kind * 7);
        float tx = t.art_wide ? 66 : 56;
        font_draw_fit(S(600, 9), tx, ry + 11, t.title, TEXT, 150 - (tx - 56));
        font_draw_fit(S(400, 9), 210, ry + 11, t.sub, MUTED, 80);
        ui_rrect(296, ry + 6, 110, 3, 1.5f, RGBA(255, 255, 255, 26));
        ui_rrect(296, ry + 6, fmaxf(3, 110 * t.frac * grow), 3, 1.5f, ACCENT);
        font_draw_right(S(700, 9), SCREEN_W - 26, ry + 11, t.value, TEXT);
    }

    Font *fh = S(400, 9);
    float hx = SCREEN_W - 16 - ui_hint_width(fh, ICON_CIRCLE, "Back");
    ui_hint(fh, hx, SCREEN_H - 9, ICON_CIRCLE, "Back", MUTED);
    hx -= 12 + ui_hint_arrows_width(fh, "Top list");
    ui_hint_arrows(fh, hx, SCREEN_H - 9, 1, "Top list", MUTED);
    hx -= 12 + font_width(fh, "L/R Period", 0);
    font_draw(fh, hx, SCREEN_H - 9, "L/R Period", MUTED, 0);
}

static void web_draw(void)
{
    Font *f_px = ui_font(FAMILY_SILKSCREEN, 400, 8), *f_h = ui_font(FAMILY_SERIF, 700, 22);
    Font *f_s = ui_font(FAMILY_SERIF, 400, 12), *f_sb = ui_font(FAMILY_SERIF, 700, 13);
    skin_web_window("BORO STATS - NAVIGATOR");
    font_draw(f_h, 24, 52, "My Listening Stats", W_MAROON, 0);
    static const char *names[PERIOD_COUNT] = { "Week", "Month", "All time" };
    float x = 456;
    for (int i = PERIOD_COUNT - 1; i >= 0; i--) {
        char t[24];
        snprintf(t, sizeof(t), i == period ? "[ %s ]" : "%s", names[i]);
        x -= font_width(f_sb, t, 0);
        font_draw(f_sb, x, 48, t, i == period ? W_BLACK : W_LINK, 0);
        x -= 10;
    }
    gfx_rect(24, 58, 432, 1, W_GRAY);
    gfx_rect(24, 59, 432, 1, W_WHITE);

    char total[24], music[24], games[24], books_t[24], line[200];
    stats_format_time(total, sizeof(total), st.ms + st.book_ms + gs.seconds * 1000);
    stats_format_time(music, sizeof(music), st.ms);
    stats_format_time(games, sizeof(games), gs.seconds * 1000);
    stats_format_time(books_t, sizeof(books_t), st.book_ms);
    font_draw(f_sb, 24, 78, period == PERIOD_ALL ? "Total, all time:" : period == PERIOD_MONTH ? "Total, 4 weeks:" : "Total this week:", W_BLACK, 0);
    float cx = skin_counter(140, 64, total);
    snprintf(line, sizeof(line), "Music %s \xC2\xB7 Games %s \xC2\xB7 Books %s", music, games, books_t);
    font_draw_fit(f_s, cx + 10, 78, line, W_BLACK, 456 - cx - 10);

    // The top list as a table.
    skin_bevel(24, 88, 212, 98, 1, W_WHITE);
    gfx_rect(26, 90, 208, 14, W_NAVY);
    char head[32];
    snprintf(head, sizeof(head), "Top %s", kind_names[top_kind]);
    font_draw(f_sb, 30, 101, head, W_WHITE, 0);
    int n = top_count(top_kind);
    if (!n) font_draw(f_s, 30, 120, "Nothing yet!", W_BLACK, 0);
    for (int i = 0; i < n && i < 5; i++) {
        Top t;
        top_item(top_kind, i, &t);
        float ry = 117 + i * 16;
        char num[8];
        snprintf(num, sizeof(num), "%d.", i + 1);
        font_draw(f_s, 30, ry, num, W_BLACK, 0);
        font_draw_fit(f_s, 44, ry, t.title, W_LINK, 130);
        gfx_rect(44, ry + 2, fminf(130, font_width(f_s, t.title, 0)), 1, W_LINK);
        font_draw_right(f_s, 230, ry, t.value, W_BLACK);
    }

    // Listening per day, like a little chart picture.
    skin_bevel(244, 88, 212, 98, 1, HEX(0xFFFFCC, 255));
    font_draw(f_sb, 250, 101, period == PERIOD_ALL ? "Listening per month" : period == PERIOD_MONTH ? "Listening per week" : "Listening per day", W_BLACK, 0);
    u32 max = 1;
    for (int i = 0; i < st.bar_count; i++) if (st.bars[i] > max) max = st.bars[i];
    float bw = 14, gap = (190 - st.bar_count * bw) / (st.bar_count > 1 ? st.bar_count - 1 : 1);
    for (int i = 0; i < st.bar_count; i++) {
        float h = (st.bars[i] ? fmaxf(2, 58.0f * st.bars[i] / max) : 2) * grow, bx = 254 + i * (bw + gap);
        gfx_rect(bx, 170 - h, bw, h, W_BLACK);
        gfx_rect(bx + 1, 171 - h, bw - 2, h - 2, i == st.today_bar ? HEX(0xFF0000, 255) : HEX(0x0000FF, 255));
        font_draw_center(f_px, bx + bw / 2, 181, st.bar_labels[i], W_BLACK);
    }

    char v[4][48], note[4][48];
    key_numbers(v, note);
    snprintf(line, sizeof(line), "Streak: %s!  \xC2\xB7  Plays: %s  \xC2\xB7  Top artist: %s  \xC2\xB7  Top station: %s", v[1], v[0], v[2], v[3]);
    font_draw_fit(f_s, 24, 202, line, W_BLACK, 432);
    font_draw(f_sb, 24, 224, "<< Back to Boro Home", W_LINK, 0);
    gfx_rect(24, 226, font_width(f_sb, "<< Back to Boro Home", 0), 1, W_LINK);
    char more[48];
    snprintf(more, sizeof(more), "DOWN: TOP %s", kind_names[(top_kind + 1) % (TOP_KINDS + 2)]);
    for (char *c = more; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    font_draw_right(f_px, 456, 238, more, W_GRAY);
    skin_web_status("L/R PERIOD   UP/DOWN TOP LIST   O BACK   DONE");
}

void stats_draw(float dt)
{
    (void)dt;
    gfx_begin(BG);
    if (skin_web()) web_draw();
    else stream_draw();
    ui_volume(S(700, 10), HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
