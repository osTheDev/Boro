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
void ui_ring(float x, float y, float w, float h, float r, u32 color); // 1px outline
void ui_shadow(float x, float y, float w, float h, float r, float blur, u32 color);
void ui_icon(int icon, float x, float y, float size, u32 color);
// Draws "78%" plus a battery outline ending at `right`, centered on cy.
void ui_battery(Font *f, float right, float cy, u32 color);
// Hint text like "✕ Play": draws the button glyph then the label. Returns end x.
float ui_hint(Font *f, float x, float baseline, int glyph, const char *label, u32 color);
float ui_hint_width(Font *f, int glyph, const char *label);
// Placeholder art: a two-color diagonal gradient picked from a seed.
void ui_placeholder_art(float x, float y, float size, float r, int seed);
void ui_placeholder_colors(int seed, u32 *c0, u32 *c1);
void ui_format_time(char *out, int size, u32 ms);
// Eases v toward target; call once per frame.
float ui_approach(float v, float target, float dt, float speed);
// The volume bar that drops in while the volume changes, in a theme's colors.
void ui_volume(Font *f, u32 panel, u32 track, u32 fill, u32 text);

#endif
