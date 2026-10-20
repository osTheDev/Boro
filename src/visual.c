#include "visual.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "library.h"
#include "player.h"
#include "settings.h"
#include "sys.h"
#include "ui.h"

#define W 240
#define H 136
#ifdef BORO_SCRIPT
#define SCENE_SECONDS 4.0f
#else
#define SCENE_SECONDS 30.0f
#endif
#define BANDS 32
#define FFT_N 256
#define MAX_PARTS 160

enum { SCENE_DOLPHINS, SCENE_PIPES, SCENE_DRIVE, SCENE_METEORS, SCENE_AURORA, SCENE_COUNT };
static const char *scene_names[SCENE_COUNT] = { "DOLPHIN BAY", "SPECTRUM PIPES", "NIGHT DRIVE", "METEOR RAIN", "AURORA RIBBON" };

typedef struct { float x, y, vx, vy, life, size; } Part;

static Texture *canvas;
static u32 L[16], fg, bg;
static float hue, t_now, scene_t, wipe, scroll, meteor_acc;
static int scene;
static Part parts[MAX_PARTS];
static int part_count;
static float bands[BANDS], peaks[24];

// ---- Colors and pixels ---------------------------------------------------------

static u32 hsl(float h, float s, float l)
{
    float c = (1 - fabsf(2 * l - 1)) * s, hp = fmodf(h, 360) / 60, x = c * (1 - fabsf(fmodf(hp, 2) - 1)), m = l - c / 2;
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    return RGBA((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255), 255);
}

// Sixteen steps from the background to the head unit color.
static void palette(void)
{
    static const u32 pal[6][2] = {
        { 0x8FDCFF, 0x04142B }, { 0xFFB547, 0x1E0F00 }, { 0x7DFF8A, 0x031A08 },
        { 0xFF6B6B, 0x200505 }, { 0xE8F0FF, 0x101418 }, { 0xC9A2FF, 0x120820 },
    };
    if (settings.deck_color == DECK_GRADIENT) {
        fg = hsl(hue, 0.95f, 0.72f);
        bg = hsl(hue, 0.8f, 0.07f);
    } else {
        fg = HEX(pal[settings.deck_color][0], 255);
        bg = HEX(pal[settings.deck_color][1], 255);
    }
    for (int k = 0; k < 16; k++) L[k] = color_lerp(bg, fg, powf(k / 15.0f, 1.3f));
}

static inline void plot(int x, int y, u32 c)
{
    if ((unsigned)x < W && (unsigned)y < H) canvas->px[y * canvas->tw + x] = c;
}

static int lvl(float k) { return k < 0 ? 0 : k > 15 ? 15 : (int)k; }

static void fill(float fx, float fy, float fw, float fh, float k)
{
    int x0 = (int)floorf(fx + 0.5f), y0 = (int)floorf(fy + 0.5f);
    int x1 = x0 + (int)fmaxf(1, floorf(fw + 0.5f)), y1 = y0 + (int)fmaxf(1, floorf(fh + 0.5f));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    u32 c = L[lvl(k)];
    for (int y = y0; y < y1; y++) {
        u32 *row = canvas->px + y * canvas->tw;
        for (int x = x0; x < x1; x++) row[x] = c;
    }
}

static void disc(float cx, float cy, float r, float k)
{
    for (int y = (int)(cy - r); y <= (int)(cy + r); y++)
        for (int x = (int)(cx - r); x <= (int)(cx + r); x++)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) plot(x, y, L[lvl(k)]);
}

static void line(float x0, float y0, float x1, float y1, float k)
{
    int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
    for (int i = 0; i <= n; i++) {
        float u = (float)i / n;
        plot((int)(x0 + (x1 - x0) * u), (int)(y0 + (y1 - y0) * u), L[lvl(k)]);
    }
}

// Blends the whole canvas toward the background, for trails.
static void fade(float a)
{
    int ia = (int)(a * 256), br = bg & 0xFF, bgc = (bg >> 8) & 0xFF, bb = (bg >> 16) & 0xFF;
    for (int y = 0; y < H; y++) {
        u32 *row = canvas->px + y * canvas->tw;
        for (int x = 0; x < W; x++) {
            u32 c = row[x];
            int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
            r += ((br - r) * ia) >> 8;
            g += ((bgc - g) * ia) >> 8;
            b += ((bb - b) * ia) >> 8;
            row[x] = RGBA(r, g, b, 255);
        }
    }
}

static float frand(void) { return (float)rand() / RAND_MAX; }

// ---- Spectrum ------------------------------------------------------------------

static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float a = -2 * 3.14159265f / len, wr = cosf(a), wi = sinf(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int j = 0; j < len / 2; j++) {
                int p = i + j, q = p + len / 2;
                float tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
                re[q] = re[p] - tr; im[q] = im[p] - ti;
                re[p] += tr; im[p] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

// Updates bands[] from the latest audio: 32 log-spaced bands, 0..1, rising
// fast and falling slowly.
static void analyze(float dt)
{
    static float re[FFT_N], im[FFT_N];
    player_scope(re, FFT_N);
    for (int i = 0; i < FFT_N; i++) {
        re[i] *= 0.5f - 0.5f * cosf(2 * 3.14159265f * i / (FFT_N - 1));
        im[i] = 0;
    }
    fft(re, im, FFT_N);
    for (int b = 0; b < BANDS; b++) {
        int lo = (int)powf(127.0f, (float)b / BANDS), hi = (int)powf(127.0f, (float)(b + 1) / BANDS);
        if (hi <= lo) hi = lo + 1;
        float e = 0;
        for (int k = lo; k < hi; k++) e = fmaxf(e, sqrtf(re[k] * re[k] + im[k] * im[k]));
        float v = log10f(1 + e * (1 + b * 0.8f)) / 1.4f;
        if (v > 1) v = 1;
        bands[b] = v > bands[b] ? v : fmaxf(v, bands[b] - dt * 1.6f);
    }
}

static float band(int i) { return bands[i < 0 ? 0 : i >= BANDS ? BANDS - 1 : i]; }

// ---- Scenes ---------------------------------------------------------------------

static void add_part(float x, float y, float vx, float vy, float size)
{
    if (part_count < MAX_PARTS) parts[part_count++] = (Part){ x, y, vx, vy, 1, size };
}

static void dolphin(float off, float x0, float span, float hgt)
{
    static const char *sprite[7] = {
        "........###.........", ".......####.........", "..##############....", ".#################..",
        "####################", "#..##############...", "#......##...........",
    };
    float p = fmodf(t_now + off, 6) / 6;
    if (p > 0.55f) return;
    float u = p / 0.55f, e = u * u * (3 - 2 * u);
    float x = x0 + e * span, y = 92 - sinf(e * 3.14159265f) * hgt, ang = -cosf(e * 3.14159265f) * 0.8f;
    float ca = cosf(ang), sa = sinf(ang);
    for (int ry = 0; ry < 7; ry++)
        for (int rx = 0; rx < 20; rx++)
            if (sprite[ry][rx] == '#') {
                float dx = rx - 10, dy = ry - 3;
                plot((int)(x + dx * ca - dy * sa), (int)(y + dx * sa + dy * ca), L[ry < 3 ? 15 : 12]);
            }
    static float last_u[2];
    int which = off > 0;
    // A splash where it leaves and re-enters the water.
    if ((last_u[which] < 0.05f) != (u < 0.05f) || (last_u[which] < 0.95f) != (u < 0.95f))
        for (int k = 0; k < 5; k++) add_part(x, 90, (frand() - 0.5f) * 30, -25 - frand() * 30, 1);
    last_u[which] = u;
}

static void scene_dolphins(float dt)
{
    for (int y = 0; y < 76; y++) fill(0, y, W, 1, 1 + y / 30.0f);
    for (int k = 0; k < 46; k++) {
        float tw = 0.5f + 0.5f * sinf(t_now * 0.9f + k * 1.7f);
        plot((k * 53) % W, (k * 29) % 62, L[lvl(4 + tw * 8)]);
    }
    disc(196, 22, 10, 13);
    disc(201, 19, 9, 2);
    for (int l = 0; l < 4; l++) {
        float base = 78 + l * 14, amp = 1.6f + l * 1.1f;
        for (int x = 0; x < W; x++) {
            float y = base + sinf(x * 0.05f + t_now * (0.6f + l * 0.25f) + l) * amp + sinf(x * 0.11f - t_now * 0.9f) * amp * 0.35f;
            fill(x, y, 1, H - y, 3 + l * 2);
            plot(x, (int)(y + 0.5f), L[lvl(8 + l * 1.5f)]);
        }
    }
    for (int k = 0; k < 8; k++) {
        float sh = 0.5f + 0.5f * sinf(t_now * 1.5f + k * 1.3f);
        fill(188 + sinf(t_now * 0.7f + k) * 5, 82 + k * 5.5f, 12 - k, 1, 6 + sh * 6);
    }
    dolphin(0, 30, 140, 50);
    dolphin(0.7f, 52, 120, 38);
    for (int i = 0; i < part_count; i++) {
        Part *p = &parts[i];
        p->life -= dt * 1.2f;
        p->vy += 90 * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        if (p->life <= 0) { parts[i--] = parts[--part_count]; continue; }
        plot((int)p->x, (int)p->y, L[lvl(8 + p->life * 7)]);
    }
}

static void scene_pipes(float dt)
{
    fill(0, 0, W, H, 0);
    float floor_y = 100, step = 9, bw = 6, x0 = (W - 24 * step) / 2;
    for (int g = 0; g < 6; g++) fill(0, floor_y + powf(g / 6.0f, 1.6f) * 36, W, 1, 2);
    for (int i = 0; i < 24; i++) {
        float v = band(i * BANDS / 24), h = v * 84, x = x0 + i * step;
        peaks[i] = fmaxf(peaks[i] - 40 * dt, h);
        for (float y = 0; y < h; y += 3) fill(x, floor_y - y - 2, bw, 2, 6 + (y / 84) * 9);
        fill(x + bw, floor_y - h, 2, h, 4);
        fill(x + 1, floor_y - h - 1, bw, 1, 15);
        fill(x, floor_y - peaks[i] - 5, bw, 1, 15);
        for (float y = 0; y < h * 0.35f; y += 2) fill(x, floor_y + 2 + y, bw, 1, 4 - y / 12);
    }
}

static void scene_drive(float dt)
{
    const float hz = 62, R = 26;
    scroll = fmodf(scroll + 0.35f * dt, 1);
    for (int y = 0; y < hz; y++) fill(0, y, W, 1, 1 + (y / hz) * 4);
    // A striped sun sinking behind the hills.
    for (int y = (int)-R; y < 0; y++) {
        float half = sqrtf(R * R - y * y), stripe = fmodf(y + R + t_now * 3, 6);
        if (!(y > -R * 0.55f && stripe < 1.5f)) fill(120 - half, hz - 4 + y, half * 2, 1, 10 + (-y / R) * 5);
    }
    for (int x = 0; x < W; x++) {
        float m = hz - 6 - fabsf(sinf(x * 0.045f) * 10 + sinf(x * 0.11f + 1) * 5);
        fill(x, m, 1, hz - m, 3);
    }
    fill(0, hz, W, H - hz, 1);
    for (int k = 0; k < 16; k++) {
        float d = fmodf(k / 16.0f + scroll, 1), y = hz + powf(d, 2.4f) * (H - hz);
        fill(0, y, W, 1, 3 + d * 9);
    }
    for (int j = -12; j <= 12; j++) line(120 + j * 3, hz, 120 + j * 34, H, 5);
    for (int k = 0; k < 6; k++) {
        float d = fmodf(k / 6.0f + scroll * 2, 1), y = hz + powf(d, 2.4f) * (H - hz), w = 1 + d * 5;
        fill(120 - w / 2, y, w, 1 + d * 4, 14);
    }
}

static void scene_meteors(float dt)
{
    fade(1 - powf(1 - 0.32f, dt * 60));
    for (int k = 0; k < 70; k++) {
        float sp = 3 + (k % 3) * 4, sx = fmodf(k * 71 + t_now * sp, W + 20), sy = fmodf(k * 37 + t_now * sp * 2, H);
        plot((int)(W - sx), (int)sy, L[2 + (k % 3) * 2]);
    }
    meteor_acc += (1 + band(2) * 3.5f) * dt;
    while (meteor_acc > 0.3f) {
        meteor_acc -= 0.3f;
        float size = 1 + (rand() % 4) * 0.5f;
        add_part(20 + frand() * W * 1.2f, -8, -(22 + frand() * 22), 40 + frand() * 40 + size * 12, size);
    }
    for (int i = 0; i < part_count; i++) {
        Part *p = &parts[i];
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        if (p->y > H + 12 || p->x < -20) { parts[i--] = parts[--part_count]; continue; }
        for (int k = 1; k <= 14; k++) plot((int)(p->x - p->vx * 0.01f * k), (int)(p->y - p->vy * 0.01f * k), L[lvl(13 - k * 0.8f)]);
        disc(p->x, p->y, p->size, 15);
    }
}

static void scene_aurora(float dt)
{
    fade(1 - powf(1 - 0.16f, dt * 60));
    for (int r = 0; r < 6; r++) {
        float amp = 8 + band(r * 5) * 38;
        for (int x = 0; x < W; x++) {
            float env = sinf((float)x / W * 3.14159265f);
            float y = 68 + sinf(x * 0.035f * (1 + r * 0.25f) + t_now * (1.1f + r * 0.45f) + r) * amp * env + sinf(x * 0.09f - t_now * 2) * 3 * env;
            fill(x, y, 1, 2, 5 + r * 2);
        }
    }
}

// ---- Frame ----------------------------------------------------------------------

void visual_enter(void)
{
    if (!canvas) canvas = tex_create(W, H);
    scene_t = 0;
    part_count = 0;
    wipe = 0;
    memset(bands, 0, sizeof(bands));
    memset(peaks, 0, sizeof(peaks));
    palette();
    fill(0, 0, W, H, 0);
}

static void draw_battery(float right, float cy, u32 c)
{
    int charging, pct = sys_battery(&charging);
    float x = right - 20;
    gfx_rect(x, cy - 4, 18, 1, c);
    gfx_rect(x, cy + 4, 18, 1, c);
    gfx_rect(x, cy - 4, 1, 9, c);
    gfx_rect(x + 17, cy - 4, 1, 9, c);
    int bars = pct < 0 ? 4 : (pct + 24) / 25;
    for (int i = 0; i < bars && i < 4; i++) gfx_rect(x + 2 + i * 4, cy - 2, 3, 5, c);
    if (pct >= 0) {
        char s[16];
        snprintf(s, sizeof(s), "%d%%", pct);
        font_draw(ui_font(FAMILY_SILKSCREEN, 400, 8), right + 4, cy + 3, s, c, 0);
    }
}

void visual_draw(float dt)
{
    static const float speeds[3] = { 5, 12, 30 };
    t_now += dt;
    hue = fmodf(hue + speeds[settings.deck_speed] * dt, 360);
    scene_t += dt;
    if (scene_t >= SCENE_SECONDS) {
        scene_t = 0;
        scene = (scene + 1) % SCENE_COUNT;
        part_count = 0;
        wipe = 1;
    }
    palette();
    analyze(dt);

    switch (scene) {
    case SCENE_DOLPHINS: scene_dolphins(dt); break;
    case SCENE_PIPES: scene_pipes(dt); break;
    case SCENE_DRIVE: scene_drive(dt); break;
    case SCENE_METEORS: scene_meteors(dt); break;
    case SCENE_AURORA: scene_aurora(dt); break;
    }
    // A bright line sweeps down when the scene changes.
    if (wipe > 0) {
        float y = (1 - wipe) * H;
        fill(0, y, W, 2, 15);
        for (float yy = y + 2; yy < H; yy += 4) fill(0, yy, W, 2, 0);
        wipe -= dt * 2.2f;
    }
    tex_upload(canvas);

    gfx_begin(bg);
    gfx_filter(0);
    gfx_tex(canvas, 0, 0, SCREEN_W, SCREEN_H, 0xFFFFFFFF);
    gfx_filter(1);

    char clock[16];
    sys_clock(clock, sizeof(clock));
    char *space = strchr(clock, ' ');
    if (space) *space = 0;
    float pulse = 0.35f + 0.35f * (0.5f + 0.5f * sinf(t_now * 3.14159265f / 2));
    font_draw(ui_font(FAMILY_VT323, 400, 26), 14, 28, clock, color_alpha(fg, pulse + 0.3f), 0);
    char name[48];
    snprintf(name, sizeof(name), "%s \xC2\xB7 %d/%d", scene_names[scene], scene + 1, SCENE_COUNT);
    Font *fs = ui_font(FAMILY_SILKSCREEN, 400, 8);
    font_draw_right(fs, SCREEN_W - 70, 19, name, color_alpha(fg, 0.7f));
    draw_battery(SCREEN_W - 40, 16, fg);

    int song = player_song();
    if (song >= 0) {
        const Song *s = &lib.songs[song];
        char info[400];
        snprintf(info, sizeof(info), "%s \xC2\xB7 %s \xC2\xB7 %s", song_title(s), song_artist(s), song_album(s));
        for (char *c = info; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        Font *f = ui_font(FAMILY_VT323, 400, 24);
        float tw = font_width(f, info, 0), span = SCREEN_W - 28;
        float off = floorf(fmodf(t_now * 40, tw + span) / 2) * 2, x = 14 + span - off;
        gfx_clip(14, SCREEN_H - 34, (int)span, 30);
        for (int k = 0; k < 4; k++) font_draw(f, x + (k & 1 ? 1.5f : -1.5f), SCREEN_H - 10 + (k & 2 ? 1.5f : -1.5f), info, bg, 0);
        font_draw(f, x, SCREEN_H - 10, info, fg, 0);
        gfx_unclip();
    }
    for (int y = 1; y < SCREEN_H; y += 2) gfx_rect(0, y, SCREEN_W, 1, RGBA(0, 0, 0, 56));
    ui_volume(ui_font(FAMILY_VT323, 400, 22), bg, L[3], fg, fg);
    gfx_end();
}
