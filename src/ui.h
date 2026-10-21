// Shared drawing pieces: fonts by family/weight/size, rounded shapes, icons,
// button glyphs and the battery readout.
#ifndef BORO_UI_H
#define BORO_UI_H

#include "font.h"
#include "gfx.h"

enum { FAMILY_SORA, FAMILY_MANROPE, FAMILY_ARCHIVO, FAMILY_SILKSCREEN, FAMILY_VT323 };
enum { ICON_PLAY, ICON_PAUSE, ICON_NEXT, ICON_PREV, ICON_SHUFFLE, ICON_REPEAT, ICON_NOTE,
       ICON_CROSS, ICON_SQUARE, ICON_CIRCLE, ICON_TRIANGLE, ICON_SPEAKER, ICON_MUTE,
       ICON_PLUS, ICON_BACKSPACE, ICON_SHIFT, ICON_COUNT };

void ui_init(void);
Font *ui_font(int family, int weight, int px);

void ui_rrect(float x, float y, float w, float h, float r, u32 color);
// Rounds off a picture's corners by drawing over them in the background color.
void ui_round_corners(float x, float y, float w, float h, int r, u32 bg);
void ui_ring(float x, float y, float w, float h, float r, u32 color); // 1px outline
void ui_shadow(float x, float y, float w, float h, float r, float blur, u32 color);
void ui_icon(int icon, float x, float y, float size, u32 color);
// Draws "78%" plus a battery outline ending at `right`, centered on cy.
void ui_battery(Font *f, float right, float cy, u32 color);
// Hint text like "✕ Play": draws the button glyph then the label. Returns end x.
float ui_hint(Font *f, float x, float baseline, int glyph, const char *label, u32 color);
float ui_hint_width(Font *f, int glyph, const char *label);
// Tiny d-pad triangles (the fonts have none), 4x7 pixels.
enum { UI_RIGHT, UI_LEFT, UI_UP, UI_DOWN };
void ui_triangle(float x, float y, int dir, u32 color);
// Hint text like "◀▶ Seek" (or "▲▼" when vertical). Returns end x.
float ui_hint_arrows(Font *f, float x, float baseline, int vertical, const char *label, u32 color);
float ui_hint_arrows_width(Font *f, const char *label);
// Placeholder art: a two-color diagonal gradient picked from a seed.
void ui_placeholder_art(float x, float y, float size, float r, int seed);
void ui_placeholder_colors(int seed, u32 *c0, u32 *c1);
void ui_format_time(char *out, int size, u32 ms);
// Eases v toward target; call once per frame.
float ui_approach(float v, float target, float dt, float speed);
// The volume bar that drops in while the volume changes, in a theme's colors.
// A glass card: dark fill and an accent hairline, faded by a (0..1).
void ui_card(float x, float y, float w, float h, float a);
// The BORO wordmark and its glowing accent dot, centered on cx at baseline y.
void ui_wordmark(Font *f, float cx, float y, float tracking, float a);
// Help shown when MUSIC, VIDEO or PICTURE has nothing in it. what finishes
// "Connect your PSP over USB and copy", path is the folder; note may be NULL.
void ui_empty_card(float cy, const char *title, const char *what, const char *path, const char *note);
// Also draws the HOLD and headphone notices, so every screen calls it last.
void ui_volume(Font *f, u32 panel, u32 track, u32 fill, u32 text);

#endif
