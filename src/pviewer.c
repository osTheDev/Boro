#include "pviewer.h"

#include <math.h>
#include <pspctrl.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "gfx.h"
#include "image.h"
#include "plib.h"
#include "ui.h"

#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define ACCENT HEX(0x5B8CFF, 255)

#define TILE 512
#define MAX_TILES 16
#define DETAIL 544     // decode so the shorter side has at least this many pixels
#define MAX_SIDE 2048  // and neither side more than this
#define BARS_SECONDS 2.0f
#define PAN_SPEED 360  // screen pixels per second at full stick

static Font *f_name, *f_small, *f_hint, *f_zoom;
static int run_first, run_count, cur;
static char context[64];

// The photo on screen, cut into textures the GU can take.
static Texture *tiles[MAX_TILES];
static int tile_count, img_w, img_h, state; // state: 0 loading, 1 shown, -1 failed

static int zoom_step;      // 0, 1, 2: 1x, 2x, 4x
static float zoom = 1;     // drawn zoom, eases toward the step
static float view_x, view_y; // image point at the middle of the screen
static float bars_t;

// Decoding happens on a worker so flipping stays responsive.
static SceUID wake = -1;
static volatile int want = -1, done_idx = -1;
static u8 *volatile done_rgba;
static volatile int done_w, done_h;

// Halves a picture until both sides fit max_side.
static u8 *fit_side(u8 *rgba, int *w, int *h)
{
    while (*w > MAX_SIDE || *h > MAX_SIDE) {
        int nw = *w / 2, nh = *h / 2;
        u8 *out = malloc(nw * nh * 4);
        if (!out) { free(rgba); return NULL; }
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    const u8 *p = rgba + ((y * 2) * *w + x * 2) * 4 + c;
                    out[(y * nw + x) * 4 + c] = (p[0] + p[4] + p[*w * 4] + p[*w * 4 + 4]) / 4;
                }
        free(rgba);
        rgba = out;
        *w = nw;
        *h = nh;
    }
    return rgba;
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(wake, 1, NULL);
        int idx = want;
        if (idx < 0) continue;
        int w = 0, h = 0;
        u8 *rgba = image_load(photos[idx].path, DETAIL, &w, &h);
        if (rgba) rgba = fit_side(rgba, &w, &h);
        if (want != idx) { free(rgba); continue; } // flipped past it meanwhile
        while (done_idx >= 0) sceKernelDelayThread(5000); // the last one hasn't been taken
        done_rgba = rgba;
        done_w = w;
        done_h = h;
        done_idx = idx;
    }
    return 0;
}

static void free_tiles(void)
{
    for (int i = 0; i < tile_count; i++) tex_destroy(tiles[i]);
    tile_count = 0;
}

static int tiles_across(void) { return (img_w + TILE - 1) / TILE; }

static void make_tiles(const u8 *rgba)
{
    int across = tiles_across(), down = (img_h + TILE - 1) / TILE;
    for (int ty = 0; ty < down; ty++)
        for (int tx = 0; tx < across; tx++) {
            int x0 = tx * TILE, y0 = ty * TILE;
            int w = img_w - x0 < TILE ? img_w - x0 : TILE, h = img_h - y0 < TILE ? img_h - y0 : TILE;
            Texture *t = tex_create(w, h);
            for (int y = 0; y < h; y++)
                memcpy(t->px + y * t->tw, rgba + ((y0 + y) * img_w + x0) * 4, w * 4);
            tex_upload(t);
            tiles[tile_count++] = t;
        }
}

static void show(int index)
{
    cur = index;
    free_tiles();
    state = 0;
    zoom_step = 0;
    zoom = 1;
    bars_t = BARS_SECONDS;
    want = run_first + cur;
    sceKernelSignalSema(wake, 1);
}

void pviewer_open(int first, int count, int index, const char *ctx)
{
    if (wake < 0) {
        f_name = ui_font(FAMILY_SORA, 700, 13);
        f_small = ui_font(FAMILY_SORA, 400, 10);
        f_hint = ui_font(FAMILY_SORA, 400, 9);
        f_zoom = ui_font(FAMILY_SORA, 700, 12);
        wake = sceKernelCreateSema("pviewer", 0, 0, 1, NULL);
        SceUID th = sceKernelCreateThread("boro_pviewer", worker, 0x24, 0x20000, PSP_THREAD_ATTR_USER, NULL);
        if (th >= 0) sceKernelStartThread(th, 0, NULL);
    }
    run_first = first;
    run_count = count;
    snprintf(context, sizeof(context), "%s", ctx);
    show(index);
}

int pviewer_current(void) { return cur; }

// Scale at 1x: the whole photo fits the screen.
static float fit_scale(void)
{
    float sx = (float)SCREEN_W / img_w, sy = (float)SCREEN_H / img_h;
    return sx < sy ? sx : sy;
}

// Keeps the screen covered by the photo where it is big enough to.
static void clamp_view(void)
{
    float s = fit_scale() * zoom;
    float hw = SCREEN_W / 2 / s, hh = SCREEN_H / 2 / s;
    view_x = img_w <= hw * 2 ? img_w / 2.0f : fminf(fmaxf(view_x, hw), img_w - hw);
    view_y = img_h <= hh * 2 ? img_h / 2.0f : fminf(fmaxf(view_y, hh), img_h - hh);
}

static void take_decoded(void)
{
    if (done_idx < 0) return;
    if (done_idx == run_first + cur && state == 0) {
        img_w = done_w;
        img_h = done_h;
        if (done_rgba) {
            make_tiles(done_rgba);
            state = 1;
            view_x = img_w / 2.0f;
            view_y = img_h / 2.0f;
        } else {
            state = -1;
        }
    }
    free(done_rgba);
    done_rgba = NULL;
    done_idx = -1;
}

int pviewer_update(const Input *in, float dt)
{
    take_decoded();
    u32 p = in->pressed;
    if (p) bars_t = BARS_SECONDS;
    else if (bars_t > 0) bars_t -= dt;

    if (p & PSP_CTRL_CIRCLE) {
        want = -1;
        free_tiles();
        return 1;
    }
    // Zoomed in, the stick pans instead of flipping.
    u32 nav = in->nav & ~(zoom_step ? in->stick_dirs : 0);
    if (nav & PSP_CTRL_LEFT && cur > 0) show(cur - 1);
    else if (nav & PSP_CTRL_RIGHT && cur < run_count - 1) show(cur + 1);
    if (state != 1) return 0;

    if (p & PSP_CTRL_RTRIGGER && zoom_step < 2) zoom_step++;
    if (p & PSP_CTRL_LTRIGGER && zoom_step > 0) zoom_step--;
    zoom = ui_approach(zoom, (float)(1 << zoom_step), dt, 12);

    float s = fit_scale() * zoom;
    int dx = in->stick_x, dy = in->stick_y;
    if (abs(dx) > 30) view_x += (dx - (dx > 0 ? 30 : -30)) / 98.0f * PAN_SPEED * dt / s;
    if (abs(dy) > 30) view_y += (dy - (dy > 0 ? 30 : -30)) / 98.0f * PAN_SPEED * dt / s;
    clamp_view();
    return 0;
}

static void draw_photo(float ox, float oy, float s)
{
    gfx_filter(1);
    int across = tiles_across();
    for (int i = 0; i < tile_count; i++) {
        const Texture *t = tiles[i];
        float x = ox + (i % across) * TILE * s, y = oy + (i / across) * TILE * s;
        // Rounded out to whole pixels so neighboring tiles meet without a seam.
        float x1 = floorf(x + t->w * s + 0.5f), y1 = floorf(y + t->h * s + 0.5f);
        x = floorf(x + 0.5f);
        y = floorf(y + 0.5f);
        if (x1 < 0 || y1 < 0 || x > SCREEN_W || y > SCREEN_H) continue;
        gfx_tex(t, x, y, x1 - x, y1 - y, 0xFFFFFFFF);
    }
}

static void draw_minimap(float a)
{
    const float bw = 58, bh = 38, right = SCREEN_W - 14, bottom = SCREEN_H - 46;
    float s = fminf(bw / img_w, bh / img_h), w = img_w * s, h = img_h * s;
    float x = right - w, y = bottom - h;
    ui_rrect(x - 2, y - 2, w + 4, h + 4, 3, color_alpha(RGBA(255, 255, 255, 200), a));
    gfx_rect(x - 1, y - 1, w + 2, h + 2, color_alpha(RGBA(0, 0, 0, 255), a));
    draw_photo(x, y, s);
    float vs = fit_scale() * zoom;
    float vw = fminf(SCREEN_W / vs, img_w) * s, vh = fminf(SCREEN_H / vs, img_h) * s;
    float vx = x + (view_x * s - vw / 2), vy = y + (view_y * s - vh / 2);
    u32 c = color_alpha(ACCENT, a);
    gfx_rect(vx, vy, vw, 2, c);
    gfx_rect(vx, vy + vh - 2, vw, 2, c);
    gfx_rect(vx, vy, 2, vh, c);
    gfx_rect(vx + vw - 2, vy, 2, vh, c);
    char label[8];
    snprintf(label, sizeof(label), "%d\xC3\x97", 1 << zoom_step);
    font_draw_right(f_zoom, x - 10, y + h / 2 + 5, label, color_alpha(TEXT, a));
}

// Hints laid out from the right edge.
static float hint_text(float x, float base, const char *key, const char *label, u32 c)
{
    char s[40];
    snprintf(s, sizeof(s), "%s %s", key, label);
    x -= font_width(f_hint, s, 0);
    font_draw(f_hint, x, base, s, c, 0);
    return x - 12;
}

static void draw_bars(void)
{
    float a = fminf(1, bars_t / 0.3f);
    if (a <= 0) return;
    for (int i = 0; i < 2; i++) {
        float y = i ? SCREEN_H - 34 : 0;
        u32 dark = RGBA(0, 0, 0, (int)(170 * a)), clear = RGBA(0, 0, 0, 0);
        if (i) gfx_gradient(0, y, SCREEN_W, 34, clear, clear, dark, dark);
        else gfx_gradient(0, y, SCREEN_W, 34, dark, dark, clear, clear);
    }
    const Photo *ph = &photos[run_first + cur];
    font_draw_fit(f_name, 14, 20, ph->name, color_alpha(TEXT, a), 260);
    char line[96];
    snprintf(line, sizeof(line), "%d of %d \xC2\xB7 %s", cur + 1, run_count, context);
    float nw = font_width(f_name, ph->name, 0);
    font_draw_fit(f_small, 14 + fminf(nw, 260) + 8, 20, line, color_alpha(MUTED, a), 150);
    ui_battery(f_small, SCREEN_W - 14, 16, color_alpha(MUTED, a));

    u32 c = color_alpha(MUTED, a);
    float base = SCREEN_H - 8, x = SCREEN_W - 14;
    x -= ui_hint_width(f_hint, ICON_CIRCLE, "Back");
    ui_hint(f_hint, x, base, ICON_CIRCLE, "Back", c);
    x -= 12;
    if (state == 1 && zoom_step) {
        x -= font_width(f_hint, "Pan", 0);
        font_draw(f_hint, x, base, "Pan", c, 0);
        // The stick: a ring with a dot.
        x -= 13;
        ui_ring(x, base - 8, 9, 9, 4.5f, c);
        ui_rrect(x + 3, base - 5, 3, 3, 1.5f, c);
        x -= 12;
    }
    if (state == 1) x = hint_text(x, base, "L/R", "Zoom", c);
    if (!zoom_step && run_count > 1) {
        x -= ui_hint_arrows_width(f_hint, "Prev / Next");
        ui_hint_arrows(f_hint, x, base, 0, "Prev / Next", c);
    }
}

void pviewer_draw(void)
{
    gfx_begin(RGBA(0, 0, 0, 255));
    if (state == 1) {
        float s = fit_scale() * zoom;
        draw_photo(SCREEN_W / 2 - view_x * s, SCREEN_H / 2 - view_y * s, s);
    } else {
        const char *msg = state ? "This photo can't be opened" : "Loading\xE2\x80\xA6";
        font_draw_center(f_small, SCREEN_W / 2, SCREEN_H / 2 + 4, msg, MUTED);
        bars_t = BARS_SECONDS; // keep the name up while there's nothing else
    }
    draw_bars();
    if (state == 1 && zoom_step) draw_minimap(1); // stays up while zoomed
    ui_volume(f_small, HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
