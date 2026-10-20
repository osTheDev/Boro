#include "settings.h"

#include <math.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"

Settings settings = { THEME_FLAC, DECK_AMBER, 1 };
const char *const theme_names[THEME_COUNT] = { "FL/AC", "iPod", "Head unit" };
const char *const deck_color_names[DECK_COLORS] = { "Ice blue", "Amber", "Green", "Red", "White", "Purple", "Gradient" };

static char path[300];

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void settings_load(const char *data_dir)
{
    snprintf(path, sizeof(path), "%s/settings.txt", data_dir);
    char buf[512];
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    // "key=value" per line.
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        int v;
        if (sscanf(line, "theme=%d", &v) == 1) settings.theme = clampi(v, 0, THEME_COUNT - 1);
        else if (sscanf(line, "deck_color=%d", &v) == 1) settings.deck_color = clampi(v, 0, DECK_COLORS - 1);
        else if (sscanf(line, "deck_speed=%d", &v) == 1) settings.deck_speed = clampi(v, 0, 2);
    }
}

void settings_save(void)
{
    char buf[128], dir[300];
    int n = snprintf(buf, sizeof(buf), "theme=%d\ndeck_color=%d\ndeck_speed=%d\n",
                     settings.theme, settings.deck_color, settings.deck_speed);
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; sceIoMkdir(dir, 0777); }
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

// ---- Screen -------------------------------------------------------------------

#define BG HEX(0x0E0F13, 255)
#define ROW_SEL HEX(0x1B1E27, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define ACCENT HEX(0x6C8CFF, 255)

enum { ROW_THEME, ROW_DECK_COLOR, ROW_COUNT };
static int sel;

static Font *S(int weight, int px) { return ui_font(FAMILY_SORA, weight, px); }

void settings_enter(void)
{
    sel = 0;
}

static void change(int dir)
{
    if (sel == ROW_THEME) settings.theme = (settings.theme + dir + THEME_COUNT) % THEME_COUNT;
    else settings.deck_color = (settings.deck_color + dir + DECK_COLORS) % DECK_COLORS;
    settings_save();
}

int settings_update(const Input *in, float dt)
{
    (void)dt;
    u32 n = in->nav, p = in->pressed;
    if (n & PSP_CTRL_UP) sel = clampi(sel - 1, 0, ROW_COUNT - 1);
    if (n & PSP_CTRL_DOWN) sel = clampi(sel + 1, 0, ROW_COUNT - 1);
    if (n & PSP_CTRL_LEFT) change(-1);
    if (n & (PSP_CTRL_RIGHT)) change(1);
    if (p & PSP_CTRL_CROSS) change(1);
    return (p & PSP_CTRL_CIRCLE) != 0;
}

void settings_draw(void)
{
    gfx_begin(BG);
    Font *title = S(700, 15), *label = S(700, 12), *help = S(400, 9), *small = S(400, 10);
    // Gear: a ring with eight teeth.
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4, cx = 24, cy = 21;
        gfx_rect(cx + cosf(a) * 7 - 1.5f, cy + sinf(a) * 7 - 1.5f, 3, 3, ACCENT);
    }
    ui_ring(19, 16, 10, 10, 5, ACCENT);
    font_draw(title, 38, 26, "Settings", TEXT, 0);
    char clock[16];
    sys_clock(clock, sizeof(clock));
    float bx = SCREEN_W - 16;
    ui_battery(small, bx, 20, MUTED);
    font_draw_right(small, bx - 56, 24, clock, MUTED);

    static const char *labels[ROW_COUNT] = { "Music theme", "Head unit color" };
    static const char *helps[ROW_COUNT] = { "How the music player looks", "Applies to every head unit screen" };
    for (int i = 0; i < ROW_COUNT; i++) {
        float y = 40 + i * 40, x = 12, w = SCREEN_W - 24, h = 36;
        if (i == sel) {
            ui_rrect(x, y, w, h, 8, ROW_SEL);
            ui_ring(x, y, w, h, 8, ACCENT);
        }
        font_draw(label, x + 12, y + 16, labels[i], TEXT, 0);
        font_draw(help, x + 12, y + 29, helps[i], MUTED, 0);
        char value[48];
        snprintf(value, sizeof(value), "%s", i == ROW_THEME ? theme_names[settings.theme] : deck_color_names[settings.deck_color]);
        Font *fv = S(700, 11);
        float vx = x + w - 12;
        // Arrows drawn as small triangles either side of the value.
        float tw = font_width(fv, value, 0);
        float lx = vx - tw - 28;
        gfx_rect(vx - 8, y + 15, 2, 6, ACCENT);
        gfx_rect(vx - 6, y + 16, 2, 4, ACCENT);
        gfx_rect(vx - 4, y + 17, 2, 2, ACCENT);
        font_draw(fv, vx - 14 - tw, y + 22, value, ACCENT, 0);
        gfx_rect(lx + 2, y + 17, 2, 2, ACCENT);
        gfx_rect(lx + 4, y + 16, 2, 4, ACCENT);
        gfx_rect(lx + 6, y + 15, 2, 6, ACCENT);
    }
    font_draw(help, 24, 140, "Screen, visualizer and video settings arrive in milestone 6.", MUTED, 0);

    Font *fh = S(400, 9);
    float x = SCREEN_W - 16 - ui_hint_width(fh, ICON_CIRCLE, "Back");
    ui_hint(fh, x, SCREEN_H - 10, ICON_CIRCLE, "Back", MUTED);
    x -= 12 + font_width(fh, "Left/Right Change", 0);
    font_draw(fh, x, SCREEN_H - 10, "Left/Right Change", MUTED, 0);
    ui_volume(S(700, 10), HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
