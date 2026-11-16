#include "settings.h"
#include "skin.h"
#include "deck.h"

#include <math.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include "about.h"
#include "player.h"
#include "plugins.h"
#include "ui.h"
#include "version.h"

Settings settings = { THEME_FLAC, DECK_AMBER, 1, 1, 1, DELAY_1M, 0, 1, FIT_FIT, HOME_STREAM, 0 };
const char *const fit_names[FIT_COUNT] = { "Fit", "Zoom", "Stretch" };
const char *const theme_names[THEME_COUNT] = { "FL/AC", "iPod", "Head unit" };
const char *const deck_color_names[DECK_COLORS] = { "Ice blue", "Amber", "Green", "Red", "White", "Purple", "Gradient" };

const char *const home_style_names[HOME_STYLES] = { "Stream", "Web" };
const float book_speeds[BOOK_SPEEDS] = { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 0.75f };
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
        else if (sscanf(line, "video_fit=%d", &v) == 1) settings.video_fit = clampi(v, 0, FIT_COUNT - 1);
        else if (sscanf(line, "book_speed=%d", &v) == 1) settings.book_speed = clampi(v, 0, BOOK_SPEEDS - 1);
        else if (sscanf(line, "home_style=%d", &v) == 1) settings.home_style = clampi(v, 0, HOME_STYLES - 1);
    }
}

void settings_save(void)
{
    char buf[320], dir[300];
    int n = snprintf(buf, sizeof(buf),
                     "theme=%d\ndeck_color=%d\ndeck_speed=%d\nkeep_on=%d\nidle_vis=%d\nidle_delay=%d\nvis_style=%d\nresume_videos=%d\nvideo_fit=%d\nhome_style=%d\nbook_speed=%d\n",
                     settings.theme, settings.deck_color, settings.deck_speed, settings.keep_on, settings.idle_vis,
                     settings.idle_delay, settings.vis_style, settings.resume_videos, settings.video_fit,
                     settings.home_style, settings.book_speed);
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

void settings_sleep_cycle(void)
{
    sleep_choice = (sleep_choice + 1) % SLEEP_CHOICES;
#ifdef BORO_SCRIPT
    sleep_left = sleep_minutes[sleep_choice];
#else
    sleep_left = sleep_minutes[sleep_choice] * 60.0f;
#endif
    player_set_gain(1);
}

int settings_sleep_left(void) { return sleep_choice ? (int)((sleep_left + 59) / 60) : 0; }

void settings_frame(float dt)
{
    if (!sleep_choice || !player_active() || player_paused()) return;
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

enum { ROW_HOME, ROW_KEEP_ON, ROW_IDLE, ROW_DELAY, ROW_STYLE, ROW_THEME, ROW_DECK_COLOR, ROW_RESUME, ROW_FIT, ROW_SLEEP, ROW_PLUGINS, ROW_ABOUT, ROW_COUNT };
static const char *const labels[ROW_COUNT] = {
    "Home style", "Keep screen on", "Idle visualizer", "Visualizer starts after", "Visualizer style",
    "Music theme", "Head unit color", "Resume videos", "Video picture", "Sleep timer", "Plugins", "About Boro",
};
static const char *const helps[ROW_COUNT] = {
    "How the home page looks: Stream with a big backdrop, or Web, an early-internet page.",
    "Screen stays lit while music plays. Off saves battery.",
    "A pixel scene that kicks in when no buttons are pressed during music.",
    "How long Boro waits before the visualizer starts.",
    "Which scene the idle visualizer shows.",
    "The look of the music player: FL/AC, iPod or Head unit.",
    "The LCD color of the Head unit theme.",
    "Pick up videos where you stopped.",
    "How videos fill the screen. SELECT changes it while watching.",
    "Fades the music out, then pauses it.",
    "Better Bright, Status, Hold+ and Snap.",
    "Version, credits and the fonts Boro uses.",
};
static const char *const groups[ROW_COUNT] = {
    "APPEARANCE", "SCREEN", "VISUALIZER", "VISUALIZER", "VISUALIZER", "MUSIC", "MUSIC", "VIDEO", "VIDEO", "MUSIC", "SYSTEM", "BORO",
};

#define ROW_Y 36
#define ROW_H 28
#define ROWS_SHOWN 7

static int sel, top, in_about, in_plugins, plugins_on;
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
    sel = top = in_about = in_plugins = 0;
    plugins_on = plugins_on_count();
    for (int i = 0; i < ROW_COUNT; i++) knob[i] = toggle_of(i) ? *toggle_of(i) : 0;
}

static int wrap(int v, int n) { return (v % n + n) % n; }

static void change(int dir)
{
    int *t = toggle_of(sel);
    if (t) *t = !*t;
    else if (sel == ROW_HOME) settings.home_style = wrap(settings.home_style + dir, HOME_STYLES);
    else if (sel == ROW_DELAY) settings.idle_delay = wrap(settings.idle_delay + dir, DELAY_COUNT);
    else if (sel == ROW_STYLE) settings.vis_style = wrap(settings.vis_style + dir, VIS_STYLES);
    else if (sel == ROW_THEME) settings.theme = wrap(settings.theme + dir, THEME_COUNT);
    else if (sel == ROW_FIT) settings.video_fit = wrap(settings.video_fit + dir, FIT_COUNT);
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
    if (in_about) {
        if (p & PSP_CTRL_CIRCLE) in_about = 0;
        return 0;
    }
    if (in_plugins) {
        if (plugins_update(in, dt)) {
            in_plugins = 0;
            plugins_on = plugins_on_count();
        }
        return 0;
    }
    if (sel == ROW_PLUGINS && p & PSP_CTRL_CROSS) {
        plugins_enter();
        in_plugins = 1;
        return 0;
    }
    if (sel == ROW_ABOUT && p & PSP_CTRL_CROSS) {
        in_about = 1;
        return 0;
    }
    if (n & PSP_CTRL_UP) sel = clampi(sel - 1, 0, ROW_COUNT - 1);
    if (n & PSP_CTRL_DOWN) sel = clampi(sel + 1, 0, ROW_COUNT - 1);
    // Stream shows the rows as chips, two to a line, four lines; Web as a form of 8.
    int shown = 8, step = skin_web() ? 1 : 2;
    if (sel < top) top = sel / step * step;
    if (sel >= top + shown) top = (sel / step * step) - shown + step;
    if (top < 0) top = 0;
    if (step == 2) top &= ~1;
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
    case ROW_HOME: snprintf(out, size, "%s", home_style_names[settings.home_style]); break;
    case ROW_DELAY: snprintf(out, size, "%s", delay_names[settings.idle_delay]); break;
    case ROW_STYLE: snprintf(out, size, "%s", vis_names[settings.vis_style]); break;
    case ROW_THEME: snprintf(out, size, "%s", theme_names[settings.theme]); break;
    case ROW_FIT: snprintf(out, size, "%s", fit_names[settings.video_fit]); break;
    case ROW_DECK_COLOR: snprintf(out, size, "%s", deck_color_names[settings.deck_color]); break;
    case ROW_ABOUT: snprintf(out, size, "v%s", BORO_VERSION); break;
    case ROW_PLUGINS: snprintf(out, size, "%d on", plugins_on); break;
    case ROW_SLEEP:
        if (!sleep_choice) snprintf(out, size, "Off");
        else snprintf(out, size, "%d min left", (int)((sleep_left + 59) / 60));
        break;
    default: *out = 0;
    }
}

// Greedy word wrap into up to two lines.
static void wrap2(Font *f, const char *text, float max_w, char *l1, char *l2, int size)
{
    l1[0] = l2[0] = 0;
    char *cur = l1;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, ' ');
        int n = e ? (int)(e - p) : (int)strlen(p);
        char trial[200];
        snprintf(trial, sizeof(trial), "%s%s%.*s", cur, cur[0] ? " " : "", n, p);
        if (cur == l1 && l1[0] && font_width(f, trial, 0) > max_w) cur = l2, snprintf(trial, sizeof(trial), "%.*s", n, p);
        else if (cur == l2 && l2[0] && font_width(f, trial, 0) > max_w) break;
        snprintf(cur, size, "%s", trial);
        p += n;
        while (*p == ' ') p++;
    }
}

static void value_text(int row, char *out, int size)
{
    if (toggle_of(row)) snprintf(out, size, "%s", *toggle_of(row) ? "On" : "Off");
    else value_of(row, out, size);
}

// The small card on the right: a sketch of the home style, the head unit
// color, or the value in big type.
static void preview(float x, float y, float w, float h)
{
    skin_glass(x, y, w, h, 0);
    float ix = x + 6, iy = y + 6, iw = w - 12, ih = h - 12;
    if (sel == ROW_HOME) {
        if (settings.home_style == HOME_WEB) {
            gfx_rect(ix, iy, iw, ih, W_TEAL);
            skin_bevel(ix + 4, iy + 4, iw - 8, ih - 8, 0, W_SILVER);
            gfx_rect(ix + 6, iy + 6, iw - 12, 6, W_NAVY);
            gfx_rect(ix + 6, iy + 13, iw - 12, ih - 21, W_WHITE);
            font_draw(ui_font(FAMILY_SERIF, 700, 11), ix + 10, iy + 26, "Welcome to Boro!", W_MAROON, 0);
            for (int i = 0; i < 4; i++) skin_bevel(ix + 10 + i * 34, iy + 32, 30, 22, i == 0, W_FACE);
            font_draw(ui_font(FAMILY_SILKSCREEN, 400, 8), ix + 10, iy + ih - 12, "THIS WEEK: 08H05M", W_BLACK, 0);
        } else {
            gfx_gradient(ix, iy, iw, ih, HEX(0x101219, 255), HEX(0x3A3F66, 255), HEX(0x0B0C10, 255), HEX(0x22263A, 255));
            font_draw(ui_font(FAMILY_SORA, 800, 8), ix + 6, iy + 12, "BORO", SK_TEXT, 2);
            font_draw(ui_font(FAMILY_SORA, 700, 12), ix + 6, iy + 30, "Night Drive", SK_TEXT, 0);
            ui_rrect(ix + 6, iy + 36, 44, 8, 4, HEX(0xF2F2F5, 230));
            for (int i = 0; i < 4; i++) {
                float tw = (iw - 12 - 9) / 4;
                ui_rrect(ix + 6 + i * (tw + 3), iy + ih - 24, tw, 18, 3, i == 0 ? HEX(0x22252F, 255) : HEX(0x181A22, 255));
                ui_ring(ix + 6 + i * (tw + 3), iy + ih - 24, tw, 18, 3, i == 0 ? SK_ACCENT : HEX(0x30323A, 255));
            }
        }
        return;
    }
    if (sel == ROW_DECK_COLOR) {
        u32 fg, bg;
        deck_colors(&fg, &bg);
        gfx_rect(ix, iy, iw, ih, bg);
        Font *f = ui_font(FAMILY_SILKSCREEN, 700, 16);
        font_draw_center(f, ix + iw / 2, iy + ih / 2 + 6, "FL/AC", fg);
        return;
    }
    char v[48];
    value_text(sel, v, sizeof(v));
    font_draw_center(ui_font(FAMILY_SORA, 400, 9), x + w / 2, y + 30, labels[sel], SK_MUTED);
    Font *big = ui_font(FAMILY_SORA, 700, 20);
    if (font_width(big, v, 0) > w - 16) big = ui_font(FAMILY_SORA, 700, 14);
    font_draw_center(big, x + w / 2, y + 60, v, SK_ACCENT);
}

static void stream_settings(void)
{
    skin_stream_back(HEX(0x6A4A8A, 255));
    skin_stream_header("Settings");
    // The setting you're on, big.
    font_draw(S(400, 9), 16, 50, groups[sel], SK_ACCENT, 1);
    font_draw_fit(S(700, 24), 16, 78, labels[sel], SK_TEXT, 270);
    char l1[160], l2[160];
    wrap2(S(400, 10), helps[sel], 270, l1, l2, sizeof(l1));
    font_draw(S(400, 10), 16, 94, l1, SK_MUTED, 0);
    font_draw(S(400, 10), 16, 106, l2, SK_MUTED, 0);
    Font *fb = S(700, 12);
    float by = 114;
    if (sel == ROW_PLUGINS || sel == ROW_ABOUT) {
        float bw = ui_hint_width(fb, ICON_CROSS, "Open") + 24;
        ui_rrect(16, by, bw, 20, 10, HEX(0xF2F2F5, 240));
        ui_hint(fb, 28, by + 14, ICON_CROSS, "Open", SK_BG);
    } else if (toggle_of(sel)) {
        float k = knob[sel];
        ui_rrect(16, by, 74, 20, 10, HEX(0xF2F2F5, 240));
        font_draw(fb, 26, by + 14, *toggle_of(sel) ? "On" : "Off", SK_BG, 0);
        ui_rrect(56, by + 3, 28, 14, 7, color_lerp(HEX(0xB8BBC6, 255), SK_ACCENT, k));
        ui_rrect(58 + 12 * k, by + 5, 10, 10, 5, 0xFFFFFFFF);
    } else {
        char v[48];
        value_of(sel, v, sizeof(v));
        float tw = font_width(fb, v, 0), bw = tw + 52;
        ui_rrect(16, by, bw, 20, 10, HEX(0xF2F2F5, 240));
        ui_triangle(28, by + 10, UI_LEFT, SK_BG);
        font_draw(fb, 42, by + 14, v, SK_BG, 0);
        ui_triangle(16 + bw - 14, by + 10, UI_RIGHT, SK_BG);
    }
    preview(300, 40, 164, 88);

    // Every setting as a chip, two to a line.
    Font *fl = S(400, 10), *flb = S(700, 10), *fv = S(700, 10);
    for (int r = 0; r < 8 && top + r < ROW_COUNT; r++) {
        int i = top + r;
        float cx = 16 + (r % 2) * 228, cy = 140 + (r / 2) * 26, cw = 220;
        skin_glass(cx, cy, cw, 22, i == sel);
        font_draw_fit(i == sel ? flb : fl, cx + 10, cy + 15, labels[i], i == sel ? SK_TEXT : SK_MUTED, 120);
        char v[48];
        value_text(i, v, sizeof(v));
        font_draw_right(fv, cx + cw - 10, cy + 15, v, i == sel ? SK_ACCENT : SK_TEXT);
    }
    if (top + 8 < ROW_COUNT) ui_triangle(SCREEN_W / 2, 247, UI_DOWN, SK_MUTED);
    if (top > 0) ui_triangle(SCREEN_W / 2, 136, UI_UP, SK_MUTED);

    Font *fh = S(400, 9);
    float hy = SCREEN_H - 9, x = SCREEN_W - 16 - ui_hint_width(fh, ICON_CIRCLE, "Back");
    ui_hint(fh, x, hy, ICON_CIRCLE, "Back", SK_MUTED);
    x -= 12 + ui_hint_arrows_width(fh, "Change");
    ui_hint_arrows(fh, x, hy, 0, "Change", SK_MUTED);
    x -= 12 + ui_hint_arrows_width(fh, "Move");
    ui_hint_arrows(fh, x, hy, 1, "Move", SK_MUTED);
}

static void web_settings(void)
{
    Font *f_px = ui_font(FAMILY_SILKSCREEN, 400, 8), *f_h = ui_font(FAMILY_SERIF, 700, 22);
    Font *f_s = ui_font(FAMILY_SERIF, 400, 12), *f_sb = ui_font(FAMILY_SERIF, 700, 13);
    skin_web_window("BORO SETTINGS - NAVIGATOR");
    font_draw(f_h, 24, 52, "Settings", W_MAROON, 0);
    font_draw(f_s, 124, 50, "Customize your Boro experience!", W_BLACK, 0);
    gfx_rect(24, 58, 432, 1, W_GRAY);
    gfx_rect(24, 59, 432, 1, W_WHITE);
    for (int r = 0; r < 8 && top + r < ROW_COUNT; r++) {
        int i = top + r;
        float y = 64 + r * 19;
        char lab[64];
        snprintf(lab, sizeof(lab), "%s:", labels[i]);
        font_draw_fit(f_sb, 30, y + 13, lab, W_BLACK, 170);
        if (toggle_of(i)) {
            for (int j = 0; j < 2; j++) {
                float cx = 210 + j * 60;
                int on = j == 0 ? *toggle_of(i) : !*toggle_of(i);
                ui_rrect(cx, y + 4, 10, 10, 5, W_DARK);
                ui_rrect(cx + 1, y + 5, 8, 8, 4, W_WHITE);
                if (on) ui_rrect(cx + 3, y + 7, 4, 4, 2, W_BLACK);
                font_draw(f_s, cx + 14, y + 13, j == 0 ? "On" : "Off", W_BLACK, 0);
            }
        } else if (i == ROW_PLUGINS || i == ROW_ABOUT) {
            skin_bevel(210, y + 1, 80, 16, 0, W_FACE);
            font_draw_center(f_s, 250, y + 13, "Open...", W_BLACK);
        } else {
            char v[48];
            value_of(i, v, sizeof(v));
            skin_bevel(210, y + 1, 130, 16, 1, W_WHITE);
            font_draw_fit(f_s, 214, y + 13, v, W_BLACK, 104);
            skin_bevel(328, y + 3, 11, 12, 0, W_SILVER);
            ui_triangle(333.5f, y + 9, UI_DOWN, W_BLACK);
        }
        if (i == sel) skin_dotted_rect(26, y, 318, 18, W_BLACK);
    }
    if (top + 8 < ROW_COUNT) font_draw(f_px, 360, 218, "MORE BELOW...", W_GRAY, 0);
    char help[200];
    snprintf(help, sizeof(help), "%s", helps[sel]);
    for (char *c = help; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    font_draw_fit(f_px, 24, 238, help, W_MAROON, 432);
    skin_web_status("X CHANGE   L/R VALUE   UP/DOWN MOVE   O BACK   DONE");
}

void settings_draw(void)
{
    if (in_about) {
        about_draw();
        return;
    }
    if (in_plugins) {
        plugins_draw();
        return;
    }
    gfx_begin(BG);
    if (skin_web()) web_settings();
    else stream_settings();
    ui_volume(S(700, 10), HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
