// Game collections: shelves you make yourself, saved in data/collections.txt
// as a [Name] line followed by one game path per line. Only Boro's own file
// changes; the games stay where they are.
#ifndef BORO_GCOLL_H
#define BORO_GCOLL_H

#define GCOLL_NAME_MAX 24

typedef struct {
    char name[GCOLL_NAME_MAX + 1];
    char **paths;
    int count, cap;
} GColl;

extern GColl *colls;
extern int coll_count;

void gcoll_load(const char *data_dir);
int gcoll_create(const char *name); // its index, or -1 if the name is taken
void gcoll_delete(int c);
int gcoll_has(int c, int game);
void gcoll_toggle(int c, int game);  // adds or removes a game
int gcoll_games(int c, int *out, int max); // the games found on the stick, as indexes into games
unsigned gcoll_seconds(int c);       // played in total

#endif
