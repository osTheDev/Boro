#include "settings.h"

#include <math.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include "player.h"
#include "ui.h"

Settings settings = { THEME_FLAC, DECK_AMBER, 1, 1, 1, DELAY_1M, 0, 1 };
const char *const theme_names[THEME_COUNT] = { "FL/AC", "iPod", "Head unit" };
const char *const deck_color_names[DECK_COLORS] = { "Ice blue", "Amber", "Green", "Red", "White", "Purple", "Gradient" };

static const char *const delay_names[DELAY_COUNT] = { "15 sec", "30 sec", "1 min", "2 min" };
#define VIS_STYLES 6
static const char *const vis_names[VIS_STYLES] = {
    "Rotate all (30 sec)", "Dolphin bay", "Spectrum pipes", "Night drive", "Meteor rain", "Aurora ribbon",
};
#define SLEEP_CHOICES 6
static const int sleep_minutes[SLEEP_CHOICES] = { 0, 15, 30, 45, 60, 90 };
#define FADE_SECONDS 10.0f

static char path[300];
static int sleep_choice;
static float sleep_left; // seconds, counting only while music plays

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
        else if (sscanf(line, "keep_on=%d", &v) == 1) settings.keep_on = v != 0;
        else if (sscanf(line, "idle_vis=%d", &v) == 1) settings.idle_vis = v != 0;
        else if (sscanf(line, "idle_delay=%d", &v) == 1) settings.idle_delay = clampi(v, 0, DELAY_COUNT - 1);
        else if (sscanf(line, "vis_style=%d", &v) == 1) settings.vis_style = clampi(v, 0, VIS_STYLES - 1);
        else if (sscanf(line, "resume_videos=%d", &v) == 1) settings.resume_videos = v != 0;
    }
}

void settings_save(void)
{
    char buf[256], dir[300];
    int n = snprintf(buf, sizeof(buf),
                     "theme=%d\ndeck_color=%d\ndeck_speed=%d\nkeep_on=%d\nidle_vis=%d\nidle_delay=%d\nvis_style=%d\nresume_videos=%d\n",
                     settings.theme, settings.deck_color, settings.deck_speed, settings.keep_on, settings.idle_vis,
                     settings.idle_delay, settings.vis_style, settings.resume_videos);
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; sceIoMkdir(dir, 0777); }
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, n);
    sceIoClose(fd);
}

float settings_idle_seconds(void)
{
#ifdef BORO_SCRIPT
    return 5.0f;
#else
    static const float secs[DELAY_COUNT] = { 15, 30, 60, 120 };
    return secs[settings.idle_delay];
#endif
}

void settings_frame(float dt)
{
    if (!sleep_choice || player_song() < 0 || player_paused()) return;
    sleep_left -= dt;
    if (sleep_left > FADE_SECONDS) return;
    if (sleep_left > 0) {
        player_set_gain(sleep_left / FADE_SECONDS);
        return;
    }
    player_toggle_pause();
    player_set_gain(1);
    sleep_choice = 0;
}

// ---- Screen -------------------------------------------------------------------

#define BG HEX(0x0E0F13, 255)
#define ROW_SEL HEX(0x1B1E27, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define ACCENT HEX(0x6C8CFF, 255)
#define TRACK HEX(0x3A3D48, 255)

enum { ROW_KEEP_ON, ROW_IDLE, ROW_DELAY, ROW_STYLE, ROW_THEME, ROW_DECK_COLOR, ROW_RESUME, ROW_SLEEP, ROW_COUNT };
static const char *const labels[ROW_COUNT] = {
    "Keep screen on", "Idle visualizer", "Visualizer starts after", "Visualizer style",
    "Music theme", "Head unit color", "Resume videos", "Sleep timer",
};
static const char *const helps[ROW_COUNT] = {
    "Screen stays lit while music plays. Off saves battery.", "Kicks in when no buttons are pressed", NULL, NULL,
    NULL, NULL, "Pick up where you stopped", "Fades the music out, then pauses it",
};

#define ROW_Y 36
#define ROW_H 28
#define ROWS_SHOWN 7

static int sel, top;
static float knob[ROW_COUNT];

static Font *S(int weight, int px) { return ui_font(FAMILY_SORA, weight, px); }

static int *toggle_of(int row)
{
    switch (row) {
    case ROW_KEEP_ON: return &settings.keep_on;
    case ROW_IDLE: return &settings.idle_vis;
    case ROW_RESUME: return &settings.resume_videos;
    default: return NULL;
    }
}

void settings_enter(void)
{
    sel = top = 0;
    for (int i = 0; i < ROW_COUNT; i++) knob[i] = toggle_of(i) ? *toggle_of(i) : 0;
}

static int wrap(int v, int n) { return (v % n + n) % n; }

static void change(int dir)
{
    int *t = toggle_of(sel);
    if (t) *t = !*t;
    else if (sel == ROW_DELAY) settings.idle_delay = wrap(settings.idle_delay + dir, DELAY_COUNT);
    else if (sel == ROW_STYLE) settings.vis_style = wrap(settings.vis_style + dir, VIS_STYLES);
    else if (sel == ROW_THEME) settings.theme = wrap(settings.theme + dir, THEME_COUNT);
    else if (sel == ROW_DECK_COLOR) settings.deck_color = wrap(settings.deck_color + dir, DECK_COLORS);
    else if (sel == ROW_SLEEP) {
        sleep_choice = wrap(sleep_choice + dir, SLEEP_CHOICES);
#ifdef BORO_SCRIPT
        sleep_left = sleep_minutes[sleep_choice]; // minutes become seconds for tests
#else
        sleep_left = sleep_minutes[sleep_choice] * 60.0f;
#endif
        player_set_gain(1);
        return; // only for this session
    }
    settings_save();
}

int settings_update(const Input *in, float dt)
{
    u32 n = in->nav, p = in->pressed;
    if (n & PSP_CTRL_UP) sel = clampi(sel - 1, 0, ROW_COUNT - 1);
    if (n & PSP_CTRL_DOWN) sel = clampi(sel + 1, 0, ROW_COUNT - 1);
    if (sel < top) top = sel;
    if (sel >= top + ROWS_SHOWN) top = sel - ROWS_SHOWN + 1;
    if (n & PSP_CTRL_LEFT) change(-1);
    if (n & PSP_CTRL_RIGHT) change(1);
    if (p & PSP_CTRL_CROSS) change(1);
    for (int i = 0; i < ROW_COUNT; i++)
        if (toggle_of(i)) knob[i] = ui_approach(knob[i], *toggle_of(i), dt, 20);
    return (p & PSP_CTRL_CIRCLE) != 0;
}

static void value_of(int row, char *out, int size)
{
    switch (row) {
    case ROW_DELAY: snprintf(out, size, "%s", delay_names[settings.idle_delay]); break;
    case ROW_STYLE: snprintf(out, size, "%s", vis_names[settings.vis_style]); break;
    case ROW_THEME: snprintf(out, size, "%s", theme_names[settings.theme]); break;
    case ROW_DECK_COLOR: snprintf(out, size, "%s", deck_color_names[settings.deck_color]); break;
    case ROW_SLEEP:
        if (!sleep_choice) snprintf(out, size, "Off");
        else snprintf(out, size, "%d min left", (int)((sleep_left + 59) / 60));
        break;
    default: *out = 0;
    }
}

void settings_draw(void)
{
    gfx_begin(BG);
    Font *title = S(700, 15), *label = S(700, 12), *help = S(400, 9), *small = S(400, 10), *fv = S(700, 11);
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

    for (int r = 0; r < ROWS_SHOWN && top + r < ROW_COUNT; r++) {
        int i = top + r;
        float y = ROW_Y + r * (ROW_H + 2), x = 12, w = SCREEN_W - 24;
        if (i == sel) {
            ui_rrect(x, y, w, ROW_H, 8, ROW_SEL);
            ui_ring(x, y, w, ROW_H, 8, ACCENT);
        }
        if (helps[i]) {
            font_draw(label, x + 10, y + 13, labels[i], TEXT, 0);
            font_draw(help, x + 10, y + 24, helps[i], MUTED, 0);
        } else {
            font_draw(label, x + 10, y + 18, labels[i], TEXT, 0);
        }
        float right = x + w - 10;
        if (toggle_of(i)) {
            float k = knob[i];
            ui_rrect(right - 30, y + 6, 30, 16, 8, color_lerp(TRACK, ACCENT, k));
            ui_rrect(right - 28 + 14 * k, y + 8, 12, 12, 6, 0xFFFFFFFF);
        } else {
            char value[48];
            value_of(i, value, sizeof(value));
            float tw = font_width(fv, value, 0);
            ui_triangle(right - 4, y + 11, UI_RIGHT, ACCENT);
            font_draw(fv, right - 10 - tw, y + 18, value, ACCENT, 0);
            ui_triangle(right - 20 - tw, y + 11, UI_LEFT, ACCENT);
        }
    }
    // Scroll hint when rows are hidden.
    if (top + ROWS_SHOWN < ROW_COUNT) font_draw_center(help, SCREEN_W / 2, ROW_Y + ROWS_SHOWN * (ROW_H + 2) + 6, "\xE2\x80\xA6", MUTED);

    Font *fh = S(400, 9);
    float hy = SCREEN_H - 8;
    float x = SCREEN_W - 16 - ui_hint_width(fh, ICON_CIRCLE, "Back");
    ui_hint(fh, x, hy, ICON_CIRCLE, "Back", MUTED);
    x -= 12 + ui_hint_arrows_width(fh, "Change");
    ui_hint_arrows(fh, x, hy, 0, "Change", MUTED);
    x -= 12 + ui_hint_width(fh, ICON_CROSS, "Toggle");
    ui_hint(fh, x, hy, ICON_CROSS, "Toggle", MUTED);
    ui_volume(S(700, 10), HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
