#include "gcoll.h"

#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "glib.h"

GColl *colls;
int coll_count;
static int coll_cap;
static char file[256];

static void add_path(GColl *c, const char *path)
{
    if (c->count == c->cap) {
        int cap = c->cap ? c->cap * 2 : 8;
        char **p = realloc(c->paths, cap * sizeof(char *));
        if (!p) return;
        c->paths = p;
        c->cap = cap;
    }
    char *s = strdup(path);
    if (s) c->paths[c->count++] = s;
}

static GColl *add_coll(const char *name)
{
    if (coll_count == coll_cap) {
        int cap = coll_cap ? coll_cap * 2 : 8;
        GColl *p = realloc(colls, cap * sizeof(GColl));
        if (!p) return NULL;
        colls = p;
        coll_cap = cap;
    }
    GColl *c = &colls[coll_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    return c;
}

static void save(void)
{
    SceUID fd = sceIoOpen(file, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    char line[300];
    for (int i = 0; i < coll_count; i++) {
        int n = snprintf(line, sizeof(line), "[%s]\n", colls[i].name);
        sceIoWrite(fd, line, n);
        for (int j = 0; j < colls[i].count; j++) {
            n = snprintf(line, sizeof(line), "%s\n", colls[i].paths[j]);
            sceIoWrite(fd, line, n);
        }
    }
    sceIoClose(fd);
}

void gcoll_load(const char *data_dir)
{
    snprintf(file, sizeof(file), "%s/collections.txt", data_dir);
    SceUID fd = sceIoOpen(file, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *buf = size > 0 ? malloc(size + 1) : NULL;
    int len = buf ? sceIoRead(fd, buf, size) : 0;
    sceIoClose(fd);
    if (!buf) return;
    buf[len < 0 ? 0 : len] = 0;
    GColl *cur = NULL;
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *cr = strchr(line, '\r');
        if (cr) *cr = 0;
        int n = strlen(line);
        if (line[0] == '[' && n > 2 && line[n - 1] == ']') {
            line[n - 1] = 0;
            cur = add_coll(line + 1);
        } else if (cur && n) {
            add_path(cur, line);
        }
        line = next;
    }
    free(buf);
}

int gcoll_create(const char *name)
{
    for (int i = 0; i < coll_count; i++)
        if (!strcasecmp(colls[i].name, name)) return -1;
    if (!add_coll(name)) return -1;
    save();
    return coll_count - 1;
}

void gcoll_delete(int c)
{
    if (c < 0 || c >= coll_count) return;
    for (int j = 0; j < colls[c].count; j++) free(colls[c].paths[j]);
    free(colls[c].paths);
    memmove(&colls[c], &colls[c + 1], (coll_count - c - 1) * sizeof(GColl));
    coll_count--;
    save();
}

static int find(int c, int game)
{
    if (c < 0 || c >= coll_count || game < 0 || game >= game_count) return -1;
    for (int j = 0; j < colls[c].count; j++)
        if (!strcasecmp(colls[c].paths[j], games[game].path)) return j;
    return -1;
}

int gcoll_has(int c, int game) { return find(c, game) >= 0; }

void gcoll_toggle(int c, int game)
{
    if (c < 0 || c >= coll_count || game < 0 || game >= game_count) return;
    int j = find(c, game);
    if (j >= 0) {
        free(colls[c].paths[j]);
        memmove(&colls[c].paths[j], &colls[c].paths[j + 1], (colls[c].count - j - 1) * sizeof(char *));
        colls[c].count--;
    } else {
        add_path(&colls[c], games[game].path);
    }
    save();
}

int gcoll_games(int c, int *out, int max)
{
    int n = 0;
    if (c < 0 || c >= coll_count) return 0;
    for (int j = 0; j < colls[c].count && n < max; j++)
        for (int g = 0; g < game_count; g++)
            if (!strcasecmp(colls[c].paths[j], games[g].path)) { out[n++] = g; break; }
    return n;
}

unsigned gcoll_seconds(int c)
{
    int idx[256], n = gcoll_games(c, idx, 256);
    unsigned s = 0;
    for (int i = 0; i < n; i++) s += games[idx[i]].seconds;
    return s;
}
