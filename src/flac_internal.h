// Shared between the FL/AC theme files (flac*.c); nothing outside them uses it.
#ifndef BORO_FLAC_INTERNAL_H
#define BORO_FLAC_INTERNAL_H

#include "flac.h"

#include <math.h>
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "library.h"
#include "player.h"
#include "playlists.h"
#include "scrub.h"
#include "stations.h"
#include "sys.h"
#include "ui.h"

#define BG HEX(0x121212, 255)
#define SIDEBAR HEX(0x000000, 255)
#define CARD HEX(0x181818, 255)
#define CARD_SEL HEX(0x282828, 255)
#define ROW_SEL HEX(0x2A2A2A, 255)
#define WHITE HEX(0xFFFFFF, 255)
#define GRAY HEX(0xB3B3B3, 255)
#define LIGHT HEX(0xE0E0E0, 255)
#define DIM HEX(0x4D4D4D, 255)
#define ACCENT HEX(0x2BD97C, 255)

#define SIDE_W 92
#define BAR_H 40
#define BAR_Y (SCREEN_H - BAR_H)
#define CONTENT_X (SIDE_W + 12)
#define CONTENT_W (SCREEN_W - SIDE_W - 24)
#define SLIDE_SECONDS 4.0f

#define HOLD_SECONDS 0.5f   // triangle held this long adds the song to a playlist
#define DELETE_SECONDS 2.0f // square held this long deletes a playlist
#define QUEUE_MAX 200

enum { SEC_HOME, SEC_ALBUMS, SEC_ARTISTS, SEC_SONGS, SEC_PLAYLISTS, SEC_RADIO, SEC_COUNT };
enum { VIEW_SECTION, VIEW_ALBUM, VIEW_ARTIST, VIEW_NOW, VIEW_PLAYLIST, VIEW_QUEUE, VIEW_PICK_ARTIST };
enum { SLIDE_ALBUM, SLIDE_SONG, SLIDE_SHUFFLE };
enum { MODAL_NONE, MODAL_ADD, MODAL_KEYBOARD, MODAL_MENU, MODAL_DELETE };
enum { MENU_RENAME, MENU_REMOVE, MENU_DELETE, MENU_COUNT };

extern int section, sidebar_focus;
extern float t_now;

// A home suggestion.
typedef struct { int kind, id; char kicker[96], title[128], sub[128]; } Slide;
extern int slide_count, slide_i;
extern float slide_t;
extern int home_sel;
extern int album_sel, album_sort, artist_sel, song_sel;
extern int page_album, page_artist, page_sel, page_pane, page_album_sel;
extern float page_scroll;
extern int playlist_sel, page_playlist, station_sel, pick_sel, queue_sel;
extern float playlist_scroll, station_scroll, pick_scroll, queue_scroll;
extern float remove_t;
extern int queue_buf[QUEUE_MAX], queue_len, queue_first;
extern int modal, add_song, add_sel, menu_sel;
enum { KB_NEW, KB_RENAME };
extern char kb_text[PLAYLIST_NAME_MAX + 1];
extern int kb_mode, kb_song, kb_row, kb_col, kb_caps;

#define play_from player_play_from

Font *F(int weight, int px);
void show_toast(const char *msg);
void push(int v);
void pop(void);
u32 darken(u32 c, float k);
void draw_art(int song, int kind, float x, float y, float size, float r, int seed);
u32 art_tint(int song, int seed);
float follow(float scroll, int sel, float row_h, float view_h, int count, float dt);
int clampi(int v, int lo, int hi);
void build_slides(void);
void play_album(int album, int start, int shuffle);
void play_in_album(int song);
void play_station(int index);
void open_playlist(int index);
void open_keyboard(int mode, const char *text, int song);
void open_album(int album);
void open_artist(int artist);
void update_section(const Input *in);
void tap_station(void);
void update_playlist_page(const Input *in);
void update_pick_artist(const Input *in);
void update_queue(const Input *in);
void update_album_page(const Input *in);
void update_artist_page(const Input *in);
void update_now(const Input *in, float dt);
void update_keyboard(const Input *in);
void update_add(const Input *in);
void update_menu(const Input *in);
void update_delete(const Input *in, float dt);
void draw_logo(float x, float baseline);
void draw_header(const char *title, int size, const char *right_hint);
void draw_home(float dt);
void draw_albums(float dt);
void draw_artists(float dt);
void draw_song_row(int song, float x, float y, float w, int selected, int number);
void draw_songs(float dt);
void draw_playlists(float dt);
void draw_radio(float dt);
void draw_hints(float right, float baseline, const int *glyphs, const char **labels, int n, Font *f);
void draw_page_header(int art_song, int seed, int round, const char *kicker, const char *title);
void draw_pills(const char *primary, const char *secondary);
void draw_album_page(float dt);
void draw_artist_page(float dt);
void draw_playlist_page(float dt);
void draw_pick_artist(float dt);
void draw_queue(float dt);
void draw_now(void);
void draw_scanning(void);
void draw_add(void);
void draw_menu(void);
void draw_delete(void);
void draw_keyboard(void);

#endif
