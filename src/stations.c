#include "stations.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "library.h"
#include "player.h"

Station *stations;
int station_count;
static int cap;

static void push(int radio, int id)
{
    if (station_count == cap) {
        cap = cap ? cap * 2 : 16;
        stations = realloc(stations, cap * sizeof(Station));
    }
    stations[station_count++] = (Station){ radio, id };
}

static void path(char *out, int size)
{
    snprintf(out, size, "%s/stations.txt", library_data_dir());
}

static void save(void)
{
    char file[300];
    int n = 0, size = 64;
    for (int i = 0; i < station_count; i++)
        if (!stations[i].radio) size += strlen(lib.artists[stations[i].id].name) + 1;
    char *buf = malloc(size);
    for (int i = 0; i < station_count; i++)
        if (!stations[i].radio) n += snprintf(buf + n, size - n, "%s\n", lib.artists[stations[i].id].name);
    sceIoMkdir(library_data_dir(), 0777);
    path(file, sizeof(file));
    SceUID fd = sceIoOpen(file, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, n);
        sceIoClose(fd);
    }
    free(buf);
}

void stations_load(void)
{
    station_count = 0;
    for (int i = 0; i < lib.radio_count; i++) push(1, i);
    // One library artist name per line, newest station first.
    char file[300];
    path(file, sizeof(file));
    SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *buf = malloc(size + 1);
    int n = buf ? sceIoRead(fd, buf, size) : -1;
    sceIoClose(fd);
    if (n < 0) { free(buf); return; }
    buf[n] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n"))
        for (int a = 0; a < lib.artist_count; a++)
            if (!strcmp(lib.artists[a].name, line)) { push(0, a); break; }
    free(buf);
}

const char *station_name(const Station *s)
{
    return s->radio ? lib.radio[s->id].name : lib.artists[s->id].name;
}

int station_art_song(const Station *s)
{
    return s->radio ? lib.radio[s->id].art_song : lib.artists[s->id].art_song;
}

void station_songs(const Station *s, int **songs, int *count)
{
    if (s->radio) { *songs = lib.radio[s->id].songs; *count = lib.radio[s->id].count; }
    else { *songs = lib.artists[s->id].songs; *count = lib.artists[s->id].song_count; }
}

int stations_add(int radio, int id)
{
    for (int i = 0; i < station_count; i++)
        if (stations[i].radio == radio && stations[i].id == id) return i;
    // New stations go right after the radio-only ones.
    push(radio, id);
    int at = lib.radio_count;
    memmove(stations + at + 1, stations + at, (station_count - 1 - at) * sizeof(Station));
    stations[at] = (Station){ radio, id };
    save();
    return at;
}

int stations_remove(int index)
{
    if (index < 0 || index >= station_count || stations[index].radio) return 0;
    memmove(stations + index, stations + index + 1, (station_count - index - 1) * sizeof(Station));
    station_count--;
    save();
    return 1;
}

int station_play(int index)
{
    const Station *st = &stations[index];
    int *songs, count;
    station_songs(st, &songs, &count);
    if (!count) return 0;
    char name[128];
    snprintf(name, sizeof(name), "%s Radio", station_name(st));
    player_play_from(FROM_RADIO, name, songs, count, -1, 1);
    player_set_locked(1);
    return 1;
}
