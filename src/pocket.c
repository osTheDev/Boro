#include "pocket.h"

#include <math.h>
#include <pspctrl.h>
#include <psphprm.h>
#include <stdio.h>
#include <string.h>
#include "font.h"
#include "gfx.h"
#include "player.h"
#include "ui.h"
#include "volume.h"

#define CARD_SECONDS 3.0f
#define PILL_SECONDS 3.0f

#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xB5B8C4, 255)
#define ACCENT HEX(0x6C8CFF, 255)
#define CARD HEX(0x1C1E28, 255)

int boroGetBrightness(void); // boro_kernel.prx
void boroSetBrightness(int level);

enum { OPEN, LOCKED, COUNTDOWN, DARK };
static int state = OPEN, brightness = -1, release_wait;
static float card_t, card_k, pill_t;
static int had_headphones = -1;
static u32 last_keys;

static int music_playing(void) { return player_song() >= 0 && !player_paused(); }

static void screen_power(int on)
{
    if (!volume_kernel()) return; // without the module the screen just goes black
    if (!on) {
        brightness = boroGetBrightness();
        boroSetBrightness(0);
    } else if (brightness > 0) {
        boroSetBrightness(brightness);
    }
}

static void remote(void)
{
    u32 keys = 0;
    sceHprmPeekCurrentKey(&keys);
    u32 down = keys & ~last_keys;
    last_keys = keys;
    if (player_song() < 0) return;
    if (down & PSP_HPRM_PLAYPAUSE) player_toggle_pause();
    if (!player_locked()) { // radio doesn't skip
        if (down & PSP_HPRM_FORWARD) player_next();
        if (down & PSP_HPRM_BACK) player_prev();
    }
}

static void headphones(void)
{
    int now = sceHprmIsHeadphoneExist() > 0;
    if (had_headphones == 1 && !now && music_playing()) {
        player_toggle_pause();
        pill_t = PILL_SECONDS;
    }
    had_headphones = now;
}

int pocket_frame(Input *in, float dt)
{
    remote();
    headphones();
    if (pill_t > 0) pill_t -= dt;

    int hold = (in->held & PSP_CTRL_HOLD) != 0;
    if (hold && state == OPEN) {
        state = music_playing() ? COUNTDOWN : LOCKED;
        card_t = CARD_SECONDS;
    } else if (!hold && state != OPEN) {
        if (state == DARK) screen_power(1);
        state = OPEN;
        release_wait = 1;
    }
    if (state == COUNTDOWN) {
        card_t -= dt;
        if (card_t <= 0) {
            state = DARK;
            screen_power(0);
        }
    }
    card_k = ui_approach(card_k, state == COUNTDOWN ? 1.0f : 0.0f, dt, 14);

    // Locked: nothing reacts, and after unlocking nothing does until every
    // button is up, so the screen comes back exactly as it was.
    if (state != OPEN) memset(in, 0, sizeof(*in));
    else if (release_wait) {
        if (in->held & ~PSP_CTRL_HOLD) memset(in, 0, sizeof(*in));
        else release_wait = 0;
    }
    return state == DARK;
}

int pocket_screen_off(void) { return state == DARK; }
int pocket_locked(void) { return state != OPEN; }

static void draw_lock_card(float a)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, (int)(90 * a)));
    const float x = 150, y = 86, w = 180, h = 100, cx = SCREEN_W / 2;
    ui_card(x, y, w, h, a);
    u32 t = color_alpha(TEXT, a), m = color_alpha(MUTED, a);
    // Padlock: shackle, then the body over its lower half.
    ui_ring(cx - 7, y + 2, 14, 20, 7, t);
    ui_ring(cx - 6, y + 3, 12, 18, 6, t);
    ui_rrect(cx - 11, y + 12, 22, 18, 3, color_alpha(CARD, a));
    ui_ring(cx - 11, y + 12, 22, 18, 3, t);
    ui_ring(cx - 10, y + 13, 20, 16, 2, t);
    gfx_rect(cx - 1, y + 18, 2, 6, t);
    font_draw_center(ui_font(FAMILY_SORA, 700, 15), cx, y + 50, "Locked", t);
    char line[32];
    int secs = (int)ceilf(card_t > 0 ? card_t : 0);
    if (secs < 1) secs = 1;
    snprintf(line, sizeof(line), "Screen off in %ds", secs);
    font_draw_center(ui_font(FAMILY_MANROPE, 500, 11), cx, y + 70, line, m);
    font_draw_center(ui_font(FAMILY_MANROPE, 500, 9), cx, y + 84, "Slide HOLD back to unlock", m);
}

static void draw_pill(float a)
{
    const float x = 140, y = 10, w = 200, h = 28;
    ui_rrect(x, y, w, h, 14, RGBA(28, 30, 40, (int)(235 * a)));
    ui_ring(x, y, w, h, 14, RGBA(108, 140, 255, (int)(140 * a)));
    gfx_rect(x + 16, y + 10, 4, 8, color_alpha(ACCENT, a));
    gfx_rect(x + 24, y + 10, 4, 8, color_alpha(ACCENT, a));
    font_draw(ui_font(FAMILY_MANROPE, 500, 11), x + 38, y + 18, "Headphones unplugged \xC2\xB7 Paused", color_alpha(TEXT, a), 0);
}

void pocket_draw(void)
{
    if (card_k > 0.01f) draw_lock_card(card_k);
    if (pill_t > 0) draw_pill(fminf(1, fminf(pill_t / 0.3f, (PILL_SECONDS - pill_t) / 0.15f)));
}
