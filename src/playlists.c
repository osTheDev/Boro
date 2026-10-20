#include "playlists.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "library.h"

Playlist *playlists;
int playlist_count;
static int cap;

static void file_path(char *out, int size, const char *name)
{
    snprintf(out, size, "%s/playlists/%s.m3u", library_data_dir(), name);
}

static int exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static void find_art(Playlist *p)
{
    static const char *ext[] = { "jpg", "jpeg", "png" };
    p->art[0] = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(p->art, sizeof(p->art), "%s/PLAYLIST ART/%s.%s", library_music_dir(), p->name, ext[i]);
        if (exists(p->art)) return;
    }
    p->art[0] = 0;
}

static int save(const Playlist *p)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/playlists", library_data_dir());
    sceIoMkdir(library_data_dir(), 0777);
    sceIoMkdir(path, 0777);
    file_path(path, sizeof(path), p->name);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    sceIoWrite(fd, "#EXTM3U\n", 8);
    for (int i = 0; i < p->count; i++) {
        const Song *s = &lib.songs[p->songs[i]];
        char line[600];
        int n = snprintf(line, sizeof(line), "#EXTINF:%u,%s - %s\n%s\n", (unsigned)(s->duration_ms / 1000),
                         song_artist(s), song_title(s), song_path(s));
        sceIoWrite(fd, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    }
    sceIoClose(fd);
    return 0;
}

static void push_song(Playlist *p, int song)
{
    if (p->count == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 16;
        p->songs = realloc(p->songs, p->cap * sizeof(int));
    }
    p->songs[p->count++] = song;
}

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(((const Playlist *)a)->name, ((const Playlist *)b)->name);
}

static int index_of(const char *name)
{
    for (int i = 0; i < playlist_count; i++)
        if (!strcasecmp(playlists[i].name, name)) return i;
    return -1;
}

static Playlist *append(const char *name)
{
    if (playlist_count == cap) {
        cap = cap ? cap * 2 : 8;
        playlists = realloc(playlists, cap * sizeof(Playlist));
    }
    Playlist *p = &playlists[playlist_count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    return p;
}

static void load_file(const char *file, const char *name)
{
    SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *buf = malloc(size + 1);
    int n = buf ? sceIoRead(fd, buf, size) : -1;
    sceIoClose(fd);
    if (n < 0) { free(buf); return; }
    buf[n] = 0;
    Playlist *p = append(name);
    // Lines are full paths (ours) or paths relative to the MUSIC folder.
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (line[0] == '#' || !line[0]) continue;
        int song = library_find_path(line);
        if (song < 0) {
            char full[600];
            snprintf(full, sizeof(full), "%s/%s", library_music_dir(), line);
            for (char *c = full; *c; c++) if (*c == '\\') *c = '/';
            song = library_find_path(full);
        }
        if (song >= 0) push_song(p, song);
    }
    free(buf);
    find_art(p);
}

void playlists_load(void)
{
    playlist_count = 0;
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/playlists", library_data_dir());
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        int len = strlen(e.d_name);
        if (len > 4 && !strcasecmp(e.d_name + len - 4, ".m3u") && len - 4 <= PLAYLIST_NAME_MAX) {
            char name[PLAYLIST_NAME_MAX + 1], file[600];
            snprintf(name, sizeof(name), "%.*s", len - 4, e.d_name);
            snprintf(file, sizeof(file), "%s/%s", dir, e.d_name);
            load_file(file, name);
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    qsort(playlists, playlist_count, sizeof(Playlist), cmp_name);
}

int playlist_create(const char *name)
{
    if (!name[0] || index_of(name) >= 0) return -1;
    Playlist *p = append(name);
    if (save(p) < 0) { playlist_count--; return -1; }
    find_art(p);
    qsort(playlists, playlist_count, sizeof(Playlist), cmp_name);
    return index_of(name);
}

int playlist_rename(int index, const char *name)
{
    Playlist *p = &playlists[index];
    int other = index_of(name);
    if (!name[0] || (other >= 0 && other != index)) return -1;
    char from[320], to[320];
    file_path(from, sizeof(from), p->name);
    file_path(to, sizeof(to), name);
    if (strcmp(from, to) && sceIoRename(from, to) < 0) {
        // Some file systems refuse renames: write the new file, drop the old.
        char old[PLAYLIST_NAME_MAX + 1];
        snprintf(old, sizeof(old), "%s", p->name);
        snprintf(p->name, sizeof(p->name), "%s", name);
        if (save(p) < 0) { snprintf(p->name, sizeof(p->name), "%s", old); return -1; }
        sceIoRemove(from);
    }
    snprintf(p->name, sizeof(p->name), "%s", name);
    find_art(p);
    qsort(playlists, playlist_count, sizeof(Playlist), cmp_name);
    return index_of(name);
}

void playlist_delete(int index)
{
    char path[320];
    file_path(path, sizeof(path), playlists[index].name);
    sceIoRemove(path);
    free(playlists[index].songs);
    memmove(playlists + index, playlists + index + 1, (playlist_count - index - 1) * sizeof(Playlist));
    playlist_count--;
}

int playlist_add(int index, int song)
{
    Playlist *p = &playlists[index];
    for (int i = 0; i < p->count; i++)
        if (p->songs[i] == song) return 0;
    push_song(p, song);
    save(p);
    return 1;
}

void playlist_remove(int index, int pos)
{
    Playlist *p = &playlists[index];
    if (pos < 0 || pos >= p->count) return;
    memmove(p->songs + pos, p->songs + pos + 1, (p->count - pos - 1) * sizeof(int));
    p->count--;
    save(p);
}

int playlist_collage(const Playlist *p, int *art_songs)
{
    int albums[4], n = 0;
    for (int i = 0; i < p->count && n < 4; i++) {
        const Song *s = &lib.songs[p->songs[i]];
        int a = s->album_id, seen = 0;
        for (int k = 0; k < n; k++) seen |= albums[k] == a;
        if (seen) continue;
        albums[n] = a;
        int art = a >= 0 ? lib.albums[a].art_song : -1;
        art_songs[n++] = art >= 0 ? art : p->songs[i];
    }
    return n;
}
