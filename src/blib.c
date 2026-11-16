#include "blib.h"

#include <ctype.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tags.h"
#include "m4b.h"

Book *books;
int book_count;

static char root[64], save_path[300], art_dir[300];
static volatile int scan_state; // 0 not started, 1 running, 2 done
static Book *found;
static int found_count, found_cap;
static int playing = -1;     // book in the player
static float save_t;
static int was_last_track;   // the player was on the book's last track

const char *blib_dir(void) { return root; }
int blib_ready(void) { return scan_state == 2; }
int blib_playing(void) { return playing; }

void blib_init(const char *data_dir, const char *books_dir)
{
    snprintf(root, sizeof(root), "%s", books_dir);
    snprintf(save_path, sizeof(save_path), "%s/books.txt", data_dir);
    snprintf(art_dir, sizeof(art_dir), "%s/bookart", data_dir);
}

// ---- Scan -------------------------------------------------------------------------

static int ends_with(const char *s, const char *end)
{
    size_t n = strlen(s), m = strlen(end);
    return n >= m && !strcasecmp(s + n - m, end);
}

// "Chapter 2" before "Chapter 10".
static int natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((u8)*a) && isdigit((u8)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((u8)*ea)) ea++;
            while (isdigit((u8)*eb)) eb++;
            if (ea - a != eb - b) return (int)(ea - a) - (int)(eb - b);
            int c = strncmp(a, b, ea - a);
            if (c) return c;
            a = ea;
            b = eb;
            continue;
        }
        int c = tolower((u8)*a) - tolower((u8)*b);
        if (c) return c;
        a++;
        b++;
    }
    return (u8)*a - (u8)*b;
}

static int cmp_name(const void *a, const void *b) { return natural_cmp(*(char *const *)a, *(char *const *)b); }

static void base_title(char *out, int size, const char *name)
{
    snprintf(out, size, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static u32 hash(const char *s)
{
    u32 h = 2166136261u;
    while (*s) h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

static int is_m4b(const char *name) { return ends_with(name, ".m4b") || ends_with(name, ".m4a"); }

// Copies a file's embedded picture out once, so it can be shown like a cover file.
static void extract_at(Book *b, const char *mp3, u32 offset, u32 size, int png)
{
    if (!size || size > 2 * 1024 * 1024) return;
    TrackInfo tmp;
    TrackInfo *ti = &tmp;
    ti->art_offset = offset;
    ti->art_size = size;
    char path[300];
    snprintf(path, sizeof(path), "%s/%08x.%s", art_dir, (unsigned)hash(b->dir), png ? "png" : "jpg");
    SceIoStat st;
    if (sceIoGetstat(path, &st) >= 0 && st.st_size > 0) { snprintf(b->cover, sizeof(b->cover), "%s", path); return; }
    u8 *buf = malloc(ti->art_size);
    if (!buf) return;
    SceUID in = sceIoOpen(mp3, PSP_O_RDONLY, 0);
    int ok = 0;
    if (in >= 0) {
        ok = sceIoLseek32(in, ti->art_offset, PSP_SEEK_SET) >= 0 && sceIoRead(in, buf, ti->art_size) == (int)ti->art_size;
        sceIoClose(in);
    }
    if (ok) {
        sceIoMkdir(art_dir, 0777);
        SceUID out = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (out >= 0) {
            ok = sceIoWrite(out, buf, ti->art_size) == (int)ti->art_size;
            sceIoClose(out);
            if (ok) snprintf(b->cover, sizeof(b->cover), "%s", path);
        }
    }
    free(buf);
}

static void extract_art(Book *b, const char *mp3, const TrackInfo *ti)
{
    if (ti->art_type != ART_JPEG && ti->art_type != ART_PNG) return;
    extract_at(b, mp3, ti->art_offset, ti->art_size, ti->art_type == ART_PNG);
}

static M4bInfo minfo; // big: one, for the scan thread

// Adds an M4B's chapters (or the whole file as one part) to the book.
static int add_m4b(Book *b, const char *file, int k, int max)
{
    if (m4b_info(file, &minfo) < 0) return k;
    if (!b->author[0]) snprintf(b->author, sizeof(b->author), "%s", minfo.author);
    if (!b->cover[0] && minfo.cover_size) extract_at(b, file, minfo.cover_offset, minfo.cover_size, minfo.cover_png);
    int n = minfo.chapter_count ? minfo.chapter_count : 1;
    for (int c = 0; c < n && k < max; c++, k++) {
        BookTrack *t = &b->tracks[k];
        memset(t, 0, sizeof(*t));
        snprintf(t->path, sizeof(t->path), "%s", file);
        t->kind = BOOK_M4B;
        t->sample_rate = minfo.rate;
        t->start_ms = minfo.chapter_count ? minfo.chapter_ms[c] : 0;
        t->end_ms = c + 1 < minfo.chapter_count ? minfo.chapter_ms[c + 1] : minfo.duration_ms;
        t->duration_ms = t->end_ms - t->start_ms;
        if (minfo.chapter_count && minfo.chapter_name[c][0]) snprintf(b->names[k], sizeof(b->names[k]), "%s", minfo.chapter_name[c]);
        else if (minfo.chapter_count) snprintf(b->names[k], sizeof(b->names[k]), "Chapter %d", k + 1);
        else {
            const char *slash = strrchr(file, '/');
            base_title(b->names[k], sizeof(b->names[k]), slash ? slash + 1 : file);
        }
        b->total_ms += t->duration_ms;
    }
    return k;
}

// Reads every track of a book; files[] are full paths in play order.
static void add_book(const char *dir, const char *title, char **files, int n, const char *cover)
{
    if (n <= 0) return;
    if (found_count == found_cap) {
        int cap = found_cap ? found_cap * 2 : 16;
        Book *grown = realloc(found, cap * sizeof(Book));
        if (!grown) return;
        found = grown;
        found_cap = cap;
    }
    if (n > BOOK_TRACKS_MAX) n = BOOK_TRACKS_MAX;
    Book *b = &found[found_count];
    memset(b, 0, sizeof(*b));
    snprintf(b->dir, sizeof(b->dir), "%s", dir);
    snprintf(b->title, sizeof(b->title), "%s", title);
    snprintf(b->cover, sizeof(b->cover), "%s", cover ? cover : "");
    // M4B chapters can outnumber the files: room for the most, trimmed after.
    b->tracks = calloc(BOOK_TRACKS_MAX, sizeof(BookTrack));
    b->names = calloc(BOOK_TRACKS_MAX, sizeof(*b->names));
    if (!b->tracks || !b->names) { free(b->tracks); free(b->names); return; }
    int k = 0;
    for (int i = 0; i < n && k < BOOK_TRACKS_MAX; i++) {
        if (is_m4b(files[i])) {
            k = add_m4b(b, files[i], k, BOOK_TRACKS_MAX);
            if (n == 1 && !cover && minfo.title[0]) snprintf(b->title, sizeof(b->title), "%s", minfo.title);
            continue;
        }
        TrackInfo ti;
        if (tags_read(files[i], &ti) < 0) continue;
        BookTrack *t = &b->tracks[k];
        snprintf(t->path, sizeof(t->path), "%s", files[i]);
        t->audio_start = ti.audio_start;
        t->audio_end = ti.audio_end;
        t->duration_ms = ti.duration_ms;
        t->total_samples = ti.total_samples;
        t->enc_delay = ti.enc_delay;
        t->sample_rate = ti.sample_rate;
        const char *slash = strrchr(files[i], '/');
        if (ti.title[0]) snprintf(b->names[k], sizeof(b->names[k]), "%s", ti.title);
        else base_title(b->names[k], sizeof(b->names[k]), slash ? slash + 1 : files[i]);
        if (!b->author[0]) snprintf(b->author, sizeof(b->author), "%s", ti.album_artist[0] ? ti.album_artist : ti.artist);
        if (!b->cover[0] && k == 0) extract_art(b, files[i], &ti);
        b->total_ms += ti.duration_ms;
        k++;
    }
    if (!k) { free(b->tracks); free(b->names); return; }
    b->track_count = k;
    BookTrack *tr = realloc(b->tracks, k * sizeof(BookTrack));
    char (*nm)[96] = realloc(b->names, k * sizeof(*b->names));
    if (tr) b->tracks = tr;
    if (nm) b->names = nm;
    // One file with a tag title reads better than the file name.
    if (n == 1 && !cover && !is_m4b(files[0])) {
        TrackInfo ti;
        if (tags_read(files[0], &ti) >= 0 && ti.album[0]) snprintf(b->title, sizeof(b->title), "%s", ti.album);
    }
    found_count++;
}

static void scan_dir(const char *dir, int depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    int cap = 64, n = 0, dn = 0;
    char **files = malloc(cap * sizeof(char *)), **dirs = malloc(cap * sizeof(char *));
    char cover[300] = "";
    SceIoDirent ent;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (ent.d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            if (dn < cap && dirs) dirs[dn++] = strdup(path);
        } else if (ends_with(ent.d_name, ".mp3") || is_m4b(ent.d_name)) {
            if (n < cap && files) files[n++] = strdup(path);
        } else if (!cover[0] && (!strncasecmp(ent.d_name, "cover.", 6) || !strncasecmp(ent.d_name, "folder.", 7)) &&
                   (ends_with(ent.d_name, ".jpg") || ends_with(ent.d_name, ".jpeg") || ends_with(ent.d_name, ".png"))) {
            snprintf(cover, sizeof(cover), "%s", path);
        }
        if (n == cap && files) { // grow
            char **g = realloc(files, cap * 2 * sizeof(char *)), **h = realloc(dirs, cap * 2 * sizeof(char *));
            if (g) files = g;
            if (h) dirs = h;
            if (g && h) cap *= 2;
        }
    }
    sceIoDclose(d);
    if (files && dirs) {
        qsort(files, n, sizeof(char *), cmp_name);
        qsort(dirs, dn, sizeof(char *), cmp_name);
        if (depth == 0) {
            // Loose files in AUDIOBOOKS: a book each.
            for (int i = 0; i < n; i++) {
                char title[96];
                const char *slash = strrchr(files[i], '/');
                base_title(title, sizeof(title), slash ? slash + 1 : files[i]);
                add_book(files[i], title, &files[i], 1, NULL);
            }
        } else if (n) {
            const char *slash = strrchr(dir, '/');
            add_book(dir, slash ? slash + 1 : dir, files, n, cover[0] ? cover : NULL);
        }
        // A folder with no MP3s of its own (an author) holds books.
        if (depth == 0 || (!n && depth < 2))
            for (int i = 0; i < dn; i++) scan_dir(dirs[i], depth + 1);
    }
    for (int i = 0; i < n; i++) free(files[i]);
    for (int i = 0; i < dn; i++) free(dirs[i]);
    free(files);
    free(dirs);
}

// ---- Saved positions -------------------------------------------------------------

static void load_positions(void)
{
    SceUID fd = sceIoOpen(save_path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *buf = size > 0 ? malloc(size + 1) : NULL;
    if (!buf) { sceIoClose(fd); return; }
    int got = sceIoRead(fd, buf, size);
    sceIoClose(fd);
    buf[got > 0 ? got : 0] = 0;
    // dir \t track \t ms \t last \t finished
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *f[5];
        int k = 0;
        for (char *p = line; k < 5 && p; k++) {
            f[k] = p;
            p = strchr(p, '\t');
            if (p) *p++ = 0;
        }
        if (k == 5)
            for (int i = 0; i < found_count; i++)
                if (!strcmp(found[i].dir, f[0])) {
                    found[i].pos_track = atoi(f[1]);
                    found[i].pos_ms = strtoul(f[2], NULL, 10);
                    snprintf(found[i].last, sizeof(found[i].last), "%s", f[3]);
                    found[i].finished = atoi(f[4]) != 0;
                    if (found[i].pos_track < 0 || found[i].pos_track >= found[i].track_count) found[i].pos_track = found[i].pos_ms = 0;
                }
        line = next;
    }
    free(buf);
}

void blib_save(void)
{
    if (!blib_ready()) return;
    SceUID fd = sceIoOpen(save_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    char line[400];
    for (int i = 0; i < book_count; i++) {
        const Book *b = &books[i];
        if (!b->last[0] && !b->pos_track && !b->pos_ms && !b->finished) continue;
        int n = snprintf(line, sizeof(line), "%s\t%d\t%u\t%s\t%d\n", b->dir, b->pos_track, (unsigned)b->pos_ms, b->last, b->finished);
        sceIoWrite(fd, line, n);
    }
    sceIoClose(fd);
}

static int scan_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    scan_dir(root, 0);
    load_positions();
    books = found;
    book_count = found_count;
    scan_state = 2;
    return sceKernelExitDeleteThread(0);
}

void blib_scan(void)
{
    if (scan_state) return;
    scan_state = 1;
    SceUID th = sceKernelCreateThread("boro_books", scan_thread, 0x30, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) scan_state = 2;
}

// ---- Playing ---------------------------------------------------------------------

u32 blib_elapsed(int book)
{
    if (book < 0 || book >= book_count) return 0;
    const Book *b = &books[book];
    if (b->finished) return b->total_ms;
    u32 ms = b->pos_ms;
    for (int i = 0; i < b->pos_track && i < b->track_count; i++) ms += b->tracks[i].duration_ms;
    return ms;
}

static void stamp(Book *b)
{
    ScePspDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    snprintf(b->last, sizeof(b->last), "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
}

void blib_play_track(int book, int track)
{
    if (book < 0 || book >= book_count) return;
    if (playing >= 0) blib_save();
    Book *b = &books[book];
    if (track < 0 || track >= b->track_count) track = 0;
    b->finished = 0;
    b->pos_track = track;
    b->pos_ms = 0;
    stamp(b);
    playing = book;
    was_last_track = 0;
    player_play_book(b->tracks, b->track_count, track, 0);
    blib_save();
}

void blib_play(int book)
{
    if (book < 0 || book >= book_count) return;
    Book *b = &books[book];
    if (b->finished) { blib_play_track(book, 0); return; }
    if (playing >= 0) blib_save();
    stamp(b);
    playing = book;
    was_last_track = 0;
    // A few seconds back, to pick up the thread.
    u32 from = b->pos_ms > 5000 ? b->pos_ms - 5000 : 0;
    player_play_book(b->tracks, b->track_count, b->pos_track, from);
    blib_save();
}

void blib_restart(int book)
{
    if (book < 0 || book >= book_count) return;
    books[book].pos_track = 0;
    books[book].pos_ms = 0;
    books[book].finished = 0;
    if (playing == book) blib_play_track(book, 0);
    else blib_save();
}

int blib_current(void)
{
    int best = -1;
    for (int i = 0; i < book_count; i++)
        if (!books[i].finished && books[i].last[0] && (best < 0 || strcmp(books[i].last, books[best].last) > 0)) best = i;
    return best;
}

void blib_frame(float dt)
{
    if (playing < 0 || playing >= book_count) return;
    Book *b = &books[playing];
    int track = player_book_track();
    if (track < 0) {
        // The player let go: the book ran out, or music took over.
        if (was_last_track && !player_active()) {
            b->finished = 1;
            b->pos_track = 0;
            b->pos_ms = 0;
            blib_save();
        } else {
            blib_save();
        }
        playing = -1;
        return;
    }
    u32 ms = player_position_ms();
    if (track != b->pos_track || ms > 0) {
        b->pos_track = track;
        b->pos_ms = ms;
    }
    was_last_track = track == b->track_count - 1 && ms + 5000 >= b->tracks[track].duration_ms;
    if (!player_paused()) save_t += dt;
    if (save_t > 20) {
        save_t = 0;
        stamp(b);
        blib_save();
    }
}
