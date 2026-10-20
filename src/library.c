#include "library.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "tags.h"

#define CACHE_MAGIC 0x4F524F42 // "BORO"
#define CACHE_VERSION 1
#define MAX_RECENT 8

Library lib;

static volatile int state = LIB_IDLE;
static volatile int scan_count;
static char data_dir[256], music_dir[256];

// String pool shared by every song. Offset 0 is the empty string.
static char *pool;
static u32 pool_len, pool_cap;

static int recent[MAX_RECENT], recent_count;

const char *lib_str(u32 offset)
{
    return pool + offset;
}

static u32 pool_add(const char *s)
{
    if (!s || !*s) return 0;
    u32 n = strlen(s) + 1;
    if (pool_len + n > pool_cap) {
        pool_cap = (pool_len + n) * 2;
        pool = realloc(pool, pool_cap);
    }
    memcpy(pool + pool_len, s, n);
    pool_len += n;
    return pool_len - n;
}

// ---- Cache --------------------------------------------------------------

typedef struct {
    Song *songs;
    int count;
    char *pool;
} OldCache;

static void cache_path(char *out, int size)
{
    snprintf(out, size, "%s/library.bin", data_dir);
}

static void load_cache(OldCache *c)
{
    memset(c, 0, sizeof(*c));
    char path[300];
    cache_path(path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    u32 head[4];
    if (sceIoRead(fd, head, sizeof(head)) == sizeof(head) && head[0] == CACHE_MAGIC &&
        head[1] == CACHE_VERSION && head[2] < 200000 && head[3] < 64 * 1024 * 1024) {
        c->songs = malloc(head[2] * sizeof(Song) + 1);
        c->pool = malloc(head[3] + 1);
        if (sceIoRead(fd, c->songs, head[2] * sizeof(Song)) == (int)(head[2] * sizeof(Song)) &&
            sceIoRead(fd, c->pool, head[3]) == (int)head[3]) {
            c->count = head[2];
        }
    }
    sceIoClose(fd);
}

static void save_cache(void)
{
    char path[300];
    sceIoMkdir(data_dir, 0777);
    cache_path(path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    u32 head[4] = { CACHE_MAGIC, CACHE_VERSION, lib.song_count, pool_len };
    sceIoWrite(fd, head, sizeof(head));
    sceIoWrite(fd, lib.songs, lib.song_count * sizeof(Song));
    sceIoWrite(fd, pool, pool_len);
    sceIoClose(fd);
}

static const char *old_pool; // qsort has no context argument
static int cmp_old(const void *a, const void *b)
{
    return strcmp(old_pool + ((const Song *)a)->path, old_pool + ((const Song *)b)->path);
}

static const Song *find_old(const OldCache *c, const char *path)
{
    int lo = 0, hi = c->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int r = strcmp(c->pool + c->songs[mid].path, path);
        if (!r) return &c->songs[mid];
        if (r < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// ---- Scanning -----------------------------------------------------------

static int song_cap;

static Song *new_song(void)
{
    if (lib.song_count == song_cap) {
        song_cap = song_cap ? song_cap * 2 : 512;
        lib.songs = realloc(lib.songs, song_cap * sizeof(Song));
    }
    Song *s = &lib.songs[lib.song_count++];
    memset(s, 0, sizeof(*s));
    return s;
}

static u32 pack_time(const ScePspDateTime *t)
{
    return (u32)(t->year - 1980) << 26 | (u32)t->month << 22 | (u32)t->day << 17 |
           (u32)t->hour << 12 | (u32)t->minute << 6 | t->second;
}

static int is_mp3(const char *name)
{
    int n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".mp3");
}

static void add_file(const char *path, const char *rel, const SceIoStat *st, const OldCache *old)
{
    u32 mtime = pack_time(&st->sce_st_mtime);
    const Song *prev = find_old(old, path);
    Song *s = new_song();
    if (prev && prev->size == (u32)st->st_size && prev->mtime == mtime) {
        *s = *prev;
        s->path = pool_add(old->pool + prev->path);
        s->title = pool_add(old->pool + prev->title);
        s->artist = pool_add(old->pool + prev->artist);
        s->album = pool_add(old->pool + prev->album);
        s->album_artist = pool_add(old->pool + prev->album_artist);
        return;
    }

    static TrackInfo t;
    if (tags_read(path, &t) < 0) {
        lib.song_count--;
        return;
    }
    s->path = pool_add(path);
    s->size = st->st_size;
    s->mtime = mtime;
    s->radio = !strncasecmp(rel, "RADIO/", 6);

    // Fall back to the file name, and for radio-only songs to the folder name.
    char name[512];
    const char *base = strrchr(path, '/');
    snprintf(name, sizeof(name), "%s", base ? base + 1 : path);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    char folder[512] = "";
    if (s->radio) {
        snprintf(folder, sizeof(folder), "%s", rel + 6);
        char *slash = strchr(folder, '/');
        if (slash) *slash = 0;
        else folder[0] = 0;
    }

    s->title = pool_add(t.title[0] ? t.title : name);
    s->artist = pool_add(t.artist[0] ? t.artist : folder[0] ? folder : "Unknown artist");
    s->album = pool_add(t.album[0] ? t.album : "Unknown album");
    s->album_artist = pool_add(s->radio && folder[0] ? folder : t.album_artist);
    s->track = t.track;
    s->disc = t.disc;
    s->art_offset = t.art_offset;
    s->art_size = t.art_size;
    s->art_type = t.art_type;
    s->duration_ms = t.duration_ms;
    s->audio_start = t.audio_start;
    s->audio_end = t.audio_end;
    s->total_samples = t.total_samples;
    s->enc_delay = t.enc_delay;
    s->enc_padding = t.enc_padding;
    s->sample_rate = t.sample_rate;
}

static void walk(const char *dir, int rel_start, const OldCache *old, int depth)
{
    if (depth > 8) return;
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    static SceIoDirent ent;
    char path[512];
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (ent.d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent.d_name);
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) {
            walk(path, rel_start, old, depth + 1);
        } else if (is_mp3(ent.d_name)) {
            add_file(path, path + rel_start, &ent.d_stat, old);
            scan_count++;
        }
    }
    sceIoDclose(d);
}

// ---- Views --------------------------------------------------------------

static const char *group_artist(const Song *s)
{
    return s->album_artist ? lib_str(s->album_artist) : lib_str(s->artist);
}

static int cmp_album_order(const void *a, const void *b)
{
    const Song *x = &lib.songs[*(const int *)a], *y = &lib.songs[*(const int *)b];
    int r = strcasecmp(group_artist(x), group_artist(y));
    if (!r) r = strcasecmp(song_album(x), song_album(y));
    if (!r) r = x->disc - y->disc;
    if (!r) r = x->track - y->track;
    if (!r) r = strcasecmp(song_title(x), song_title(y));
    return r;
}

static int cmp_title(const void *a, const void *b)
{
    const Song *x = &lib.songs[*(const int *)a], *y = &lib.songs[*(const int *)b];
    int r = strcasecmp(song_title(x), song_title(y));
    return r ? r : strcasecmp(song_artist(x), song_artist(y));
}

static int cmp_album_name(const void *a, const void *b)
{
    const Album *x = &lib.albums[*(const int *)a], *y = &lib.albums[*(const int *)b];
    int r = strcasecmp(x->name, y->name);
    return r ? r : strcasecmp(x->artist, y->artist);
}

static int cmp_radio(const void *a, const void *b)
{
    const Song *x = &lib.songs[*(const int *)a], *y = &lib.songs[*(const int *)b];
    return strcasecmp(group_artist(x), group_artist(y));
}

// Albums are stored grouped by artist (artists A-Z, then album A-Z), so
// artist ids and albums_by_artist are in that order too.
static void build_views(void)
{
    int n = lib.song_count, browsable = 0, radio_n = 0;
    int *order = malloc((n + 1) * sizeof(int)), *radio_order = malloc((n + 1) * sizeof(int));
    for (int i = 0; i < n; i++) {
        lib.songs[i].album_id = lib.songs[i].artist_id = lib.songs[i].radio_id = -1;
        if (lib.songs[i].radio) radio_order[radio_n++] = i;
        else order[browsable++] = i;
    }

    // Albums: consecutive runs of the same artist + album name.
    qsort(order, browsable, sizeof(int), cmp_album_order);
    lib.albums = calloc(browsable + 1, sizeof(Album));
    lib.album_count = 0;
    for (int i = 0; i < browsable;) {
        const Song *first = &lib.songs[order[i]];
        int j = i + 1;
        while (j < browsable && !strcasecmp(group_artist(&lib.songs[order[j]]), group_artist(first)) &&
               !strcasecmp(song_album(&lib.songs[order[j]]), song_album(first)))
            j++;
        Album *a = &lib.albums[lib.album_count];
        a->name = song_album(first);
        a->artist = group_artist(first);
        a->songs = &order[i];
        a->count = j - i;
        a->art_song = -1;
        for (int k = i; k < j; k++) {
            lib.songs[order[k]].album_id = lib.album_count;
            if (a->art_song < 0 && lib.songs[order[k]].art_type) a->art_song = order[k];
        }
        lib.album_count++;
        i = j;
    }

    // Artists: albums are grouped by artist already (order is artist-first).
    lib.artists = calloc(lib.album_count + 1, sizeof(Artist));
    lib.artist_count = 0;
    lib.albums_by_artist = malloc((lib.album_count + 1) * sizeof(int));
    for (int i = 0; i < lib.album_count; i++) lib.albums_by_artist[i] = i;
    for (int i = 0; i < lib.album_count;) {
        int j = i + 1;
        while (j < lib.album_count && !strcasecmp(lib.albums[j].artist, lib.albums[i].artist)) j++;
        Artist *ar = &lib.artists[lib.artist_count];
        ar->name = lib.albums[i].artist;
        ar->albums = &lib.albums_by_artist[i];
        ar->album_count = j - i;
        ar->songs = lib.albums[i].songs;
        ar->song_count = 0;
        ar->art_song = -1;
        for (int k = i; k < j; k++) {
            lib.albums[k].artist_id = lib.artist_count;
            ar->song_count += lib.albums[k].count;
            if (ar->art_song < 0) ar->art_song = lib.albums[k].art_song;
            for (int s = 0; s < lib.albums[k].count; s++) lib.songs[lib.albums[k].songs[s]].artist_id = lib.artist_count;
        }
        lib.artist_count++;
        i = j;
    }

    // Album ids follow artist order; library_albums_sorted() gives A-Z by name.
    lib.songs_az = malloc((browsable + 1) * sizeof(int));
    memcpy(lib.songs_az, order, browsable * sizeof(int));
    qsort(lib.songs_az, browsable, sizeof(int), cmp_title);
    lib.songs_az_count = browsable;

    // Radio-only artists.
    qsort(radio_order, radio_n, sizeof(int), cmp_radio);
    lib.radio = calloc(radio_n + 1, sizeof(RadioArtist));
    lib.radio_count = 0;
    for (int i = 0; i < radio_n;) {
        int j = i + 1;
        while (j < radio_n && !strcasecmp(group_artist(&lib.songs[radio_order[j]]), group_artist(&lib.songs[radio_order[i]]))) j++;
        RadioArtist *r = &lib.radio[lib.radio_count];
        r->name = group_artist(&lib.songs[radio_order[i]]);
        r->songs = &radio_order[i];
        r->count = j - i;
        for (int k = i; k < j; k++) lib.songs[radio_order[k]].radio_id = lib.radio_count;
        lib.radio_count++;
        i = j;
    }
}

static int *albums_az;

int *library_albums_sorted(int by_artist)
{
    if (by_artist) return lib.albums_by_artist;
    if (!albums_az) {
        albums_az = malloc((lib.album_count + 1) * sizeof(int));
        for (int i = 0; i < lib.album_count; i++) albums_az[i] = i;
        qsort(albums_az, lib.album_count, sizeof(int), cmp_album_name);
    }
    return albums_az;
}

// ---- Recent albums --------------------------------------------------------

static void recent_path(char *out, int size)
{
    snprintf(out, size, "%s/recent.txt", data_dir);
}

static void load_recent(void)
{
    char path[300], buf[4096];
    recent_path(path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = 0;
    // Each line: "<album>\t<artist>"
    for (char *line = strtok(buf, "\n"); line && recent_count < MAX_RECENT; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        for (int a = 0; a < lib.album_count; a++)
            if (!strcmp(lib.albums[a].name, line) && !strcmp(lib.albums[a].artist, tab + 1)) {
                recent[recent_count++] = a;
                break;
            }
    }
}

static void save_recent(void)
{
    char path[300], buf[4096];
    int n = 0;
    for (int i = 0; i < recent_count; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s\t%s\n", lib.albums[recent[i]].name, lib.albums[recent[i]].artist);
    sceIoMkdir(data_dir, 0777);
    recent_path(path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

int library_recent(int *out, int max)
{
    int n = recent_count < max ? recent_count : max;
    memcpy(out, recent, n * sizeof(int));
    return n;
}

void library_note_played(int song)
{
    if (song < 0 || song >= lib.song_count) return;
    int album = lib.songs[song].album_id;
    if (album < 0 || (recent_count && recent[0] == album)) return;
    int i = 0;
    while (i < recent_count && recent[i] != album) i++;
    if (i == recent_count && recent_count < MAX_RECENT) recent_count++;
    if (i == MAX_RECENT) i = MAX_RECENT - 1;
    memmove(recent + 1, recent, i * sizeof(int));
    recent[0] = album;
    save_recent();
}

// ---- Thread ---------------------------------------------------------------

static int scan_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    OldCache old;
    load_cache(&old);
    old_pool = old.pool;
    qsort(old.songs, old.count, sizeof(Song), cmp_old);

    pool_cap = 64 * 1024;
    pool = malloc(pool_cap);
    pool[0] = 0; // offset 0 is the empty string
    pool_len = 1;
    walk(music_dir, strlen(music_dir) + 1, &old, 0);
    free(old.songs);
    free(old.pool);

    save_cache();
    build_views();
    load_recent();
    state = LIB_READY;
    sceKernelExitDeleteThread(0);
    return 0;
}

void library_start(const char *data, const char *music)
{
    snprintf(data_dir, sizeof(data_dir), "%s", data);
    snprintf(music_dir, sizeof(music_dir), "%s", music);
    state = LIB_SCANNING;
    SceUID th = sceKernelCreateThread("boro_scan", scan_thread, 0x30, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    else state = LIB_READY;
}

int library_state(void)
{
    return state;
}

int library_scan_count(void)
{
    return scan_count;
}
