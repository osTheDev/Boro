#include "splash.h"

#include <math.h>
#include "font.h"
#include "gfx.h"
#include "home.h"
#include "library.h"
#include "ui.h"

#define MIN_SECONDS 0.8f
#define MAX_SECONDS 3.0f

static float t, bar;

int splash_update(float dt)
{
    t += dt;
    int ready = library_state() == LIB_READY;
    // Creeps toward 90% while loading, then fills.
    float target = ready ? 1.0f : fminf(0.9f, t / 1.2f);
    bar = ui_approach(bar, target, dt, 8);
    return (ready && t >= MIN_SECONDS && bar > 0.98f) || t >= MAX_SECONDS;
}

void splash_draw(void)
{
    gfx_begin(HEX(0x0B0C10, 255));
    home_backdrop();
    ui_wordmark(ui_font(FAMILY_SORA, 800, 40), SCREEN_W / 2, 148, 12, 1);
    font_draw_center(ui_font(FAMILY_MANROPE, 500, 11), SCREEN_W / 2, 186, "Loading library\xE2\x80\xA6", HEX(0xB5B8C4, 255));
    ui_rrect(200, 196, 80, 2, 1, RGBA(255, 255, 255, 40));
    ui_rrect(200, 196, 80 * bar, 2, 1, HEX(0x6C8CFF, 255));
    gfx_end();
}
