#include "about.h"

#include "font.h"
#include "gfx.h"
#include "ui.h"
#include "version.h"

#define BG HEX(0x101115, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xB5B8C4, 255)
#define ACCENT HEX(0x6C8CFF, 255)

void about_draw(void)
{
    static Texture *glow;
    if (!glow) glow = tex_glow(128);
    gfx_begin(BG);
    gfx_additive(1);
    gfx_tex(glow, 40, -40, 400, 160, color_alpha(ACCENT, 0.18f));
    gfx_additive(0);
    font_draw(ui_font(FAMILY_SORA, 700, 15), 14, 24, "About", TEXT, 0);
    ui_battery(ui_font(FAMILY_SORA, 400, 10), SCREEN_W - 16, 20, MUTED);

    ui_wordmark(ui_font(FAMILY_SORA, 800, 26), SCREEN_W / 2, 66, 8, 1);
    Font *body = ui_font(FAMILY_MANROPE, 500, 11), *key = ui_font(FAMILY_MANROPE, 700, 11);
    font_draw_center(body, SCREEN_W / 2, 88, "Version " BORO_VERSION "  \xC2\xB7  Music, video and photos for PSP", MUTED);
    gfx_rect(40, 98, 400, 1, RGBA(255, 255, 255, 30));

    static const char *const rows[][2] = {
        { "Made by", "Bingo Bob" },
        { "Built with", "PSPSDK \xC2\xB7 FreeType \xC2\xB7 libjpeg \xC2\xB7 libpng \xC2\xB7 zlib \xC2\xB7 bzip2" },
        { "Source", "github.com/osTheDev/Boro" },
    };
    float y = 118;
    for (int i = 0; i < 3; i++, y += 18) {
        font_draw(key, 40, y, rows[i][0], MUTED, 0);
        font_draw(body, 130, y, rows[i][1], TEXT, 0);
    }
    font_draw(key, 40, y + 6, "Fonts", MUTED, 0);
    font_draw(body, 130, y + 6, "Sora, Manrope, Archivo, Silkscreen, VT323", TEXT, 0);
    font_draw(ui_font(FAMILY_MANROPE, 500, 10), 130, y + 22, "SIL Open Font License 1.1. Full text in the Boro folder.", MUTED, 0);

    Font *hint = ui_font(FAMILY_SORA, 400, 9);
    ui_hint(hint, SCREEN_W - 16 - ui_hint_width(hint, ICON_CIRCLE, "Back"), SCREEN_H - 8, ICON_CIRCLE, "Back", MUTED);
    ui_volume(ui_font(FAMILY_SORA, 700, 10), HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
