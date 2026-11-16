// The Boro skin, shared by the home page, Stats and Settings: Stream (soft
// backdrop, glass cards) or Web (an early-internet Navigator window), picked
// in Settings > Home style.
#ifndef BORO_SKIN_H
#define BORO_SKIN_H

#include <psptypes.h>
#include "font.h"

#define SK_BG HEX(0x0B0C10, 255)
#define SK_TEXT HEX(0xF2F2F5, 255)
#define SK_MUTED HEX(0x9A9DAA, 255)
#define SK_ACCENT HEX(0x6C8CFF, 255)

#define W_TEAL HEX(0x008080, 255)
#define W_TEAL2 HEX(0x007676, 255)
#define W_FACE HEX(0xD4D0C8, 255)
#define W_SILVER HEX(0xC0C0C0, 255)
#define W_NAVY HEX(0x000080, 255)
#define W_WHITE HEX(0xFFFFFF, 255)
#define W_BLACK HEX(0x000000, 255)
#define W_DARK HEX(0x404040, 255)
#define W_GRAY HEX(0x808080, 255)
#define W_LINK HEX(0x0000EE, 255)
#define W_MAROON HEX(0x800000, 255)

int skin_web(void); // the Web style is picked

// Stream: a soft glow of color top right, shaded toward the text and the bottom.
void skin_stream_back(u32 glow);
// "BORO › <page>" and the battery.
void skin_stream_header(const char *page);
void skin_glass(float x, float y, float w, float h, int on);
// A rounded chip; returns its width.
float skin_pill(Font *f, float x, float y, const char *text, int on);
float skin_pill_width(Font *f, const char *text);

// Web: the teal desktop, the window with its title bar and the white page.
void skin_web_window(const char *title);
void skin_web_status(const char *text); // the status bar at the bottom
void skin_bevel(float x, float y, float w, float h, int down, u32 face);
void skin_dotted_rect(float x, float y, float w, float h, u32 c);
// A green-on-black hit counter; returns where it ends.
float skin_counter(float x, float y, const char *digits);

#endif
