#include "gfx.h"

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>

#define BUF_W 512

static unsigned int __attribute__((aligned(16))) list[262144];
static const Texture *bound;
static u32 __attribute__((aligned(16))) alpha_clut[256]; // white, alpha = index

typedef struct { float u, v; u32 color; float x, y, z; } TVert;
typedef struct { u32 color; float x, y, z; } CVert;

#define TVERT_FMT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)
#define CVERT_FMT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

u32 color_alpha(u32 c, float a)
{
    u32 al = (u32)clampf((c >> 24) * a, 0, 255);
    return (c & 0x00FFFFFF) | (al << 24);
}

u32 color_lerp(u32 a, u32 b, float t)
{
    u32 out = 0;
    for (int s = 0; s < 32; s += 8) {
        float ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        out |= ((u32)(ca + (cb - ca) * t) & 0xFF) << s;
    }
    return out;
}

static int pow2(int v)
{
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

Texture *tex_create(int w, int h)
{
    Texture *t = calloc(1, sizeof(Texture));
    t->w = w;
    t->h = h;
    t->tw = pow2(w);
    t->th = pow2(h);
    t->px = memalign(16, t->tw * t->th * 4);
    memset(t->px, 0, t->tw * t->th * 4);
    return t;
}

Texture *tex_create_alpha8(int w, int h)
{
    Texture *t = calloc(1, sizeof(Texture));
    t->w = w;
    t->h = h;
    t->tw = pow2(w);
    t->th = pow2(h);
    t->alpha8 = 1;
    t->px = memalign(16, t->tw * t->th);
    memset(t->px, 0, t->tw * t->th);
    return t;
}

void tex_destroy(Texture *t)
{
    if (!t) return;
    if (bound == t) bound = NULL;
    free(t->px);
    free(t);
}

void tex_upload(Texture *t)
{
    tex_upload_rows(t, 0, t->th);
}

void tex_upload_rows(Texture *t, int y, int h)
{
    int bpp = t->alpha8 ? 1 : 4;
    sceKernelDcacheWritebackRange((char *)t->px + y * t->tw * bpp, h * t->tw * bpp);
    if (bound == t) bound = NULL;
}

static void put_alpha(Texture *t, int x, int y, float a)
{
    u32 *p = &t->px[y * t->tw + x];
    u32 al = (u32)(clampf(a, 0, 1) * 255.0f + 0.5f);
    if (al > (*p >> 24)) *p = 0x00FFFFFF | (al << 24);
}

// Signed distance from (px, py) to a rounded box centered at (cx, cy).
static float sd_round_box(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

Texture *tex_glow(int size)
{
    Texture *t = tex_create(size, size);
    float c = size * 0.5f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float dx = (x + 0.5f - c) / c, dy = (y + 0.5f - c) / c;
            float d = clampf(1.0f - sqrtf(dx * dx + dy * dy), 0, 1);
            put_alpha(t, x, y, d * d * (3 - 2 * d));
        }
    tex_upload(t);
    return t;
}

Texture *tex_round_fill(float r)
{
    int b = (int)ceilf(r), s = 2 * b + 2;
    Texture *t = tex_create(s, s);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++)
            put_alpha(t, x, y, 0.5f - sd_round_box(x + 0.5f, y + 0.5f, s * 0.5f, s * 0.5f, s * 0.5f, s * 0.5f, r));
    tex_upload(t);
    return t;
}

Texture *tex_round_ring(float r, float width)
{
    int b = (int)ceilf(r), s = 2 * b + 2;
    Texture *t = tex_create(s, s);
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float d = sd_round_box(x + 0.5f, y + 0.5f, s * 0.5f, s * 0.5f, s * 0.5f, s * 0.5f, r);
            put_alpha(t, x, y, 0.5f - (fabsf(d + width * 0.5f) - width * 0.5f));
        }
    tex_upload(t);
    return t;
}

Texture *tex_round_shadow(float r, float blur, int hollow)
{
    int b = (int)ceilf(r + blur), s = 2 * b + 2;
    Texture *t = tex_create(s, s);
    float h = s * 0.5f - blur;
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            float d = sd_round_box(x + 0.5f, y + 0.5f, s * 0.5f, s * 0.5f, h, h, r);
            float k = 1.0f - clampf(d / blur, 0, 1);
            put_alpha(t, x, y, k * k * k * (hollow ? clampf(d + 0.5f, 0, 1) : 1.0f));
        }
    tex_upload(t);
    return t;
}

Texture *tex_round_gradient(int w, int h, float r, u32 c0, u32 c1)
{
    Texture *t = tex_create(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float k = clampf(((float)x / w + (float)y / h) * 0.5f, 0, 1);
            float a = clampf(0.5f - sd_round_box(x + 0.5f, y + 0.5f, w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, r), 0, 1);
            t->px[y * t->tw + x] = color_alpha(color_lerp(c0, c1, k) | 0xFF000000, a);
        }
    tex_upload(t);
    return t;
}

static float seg_dist(float px, float py, float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    float k = len2 > 0 ? clampf(((px - x0) * dx + (py - y0) * dy) / len2, 0, 1) : 0;
    float ex = px - (x0 + dx * k), ey = py - (y0 + dy * k);
    return sqrtf(ex * ex + ey * ey);
}

void tex_stroke_line(Texture *t, float x0, float y0, float x1, float y1, float width)
{
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++)
            put_alpha(t, x, y, width * 0.5f + 0.5f - seg_dist(x + 0.5f, y + 0.5f, x0, y0, x1, y1));
}

void tex_stroke_circle(Texture *t, float cx, float cy, float r, float width)
{
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            put_alpha(t, x, y, width * 0.5f + 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - r));
        }
}

void tex_fill_circle(Texture *t, float cx, float cy, float r)
{
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            put_alpha(t, x, y, r + 0.5f - sqrtf(dx * dx + dy * dy));
        }
}

void tex_fill_poly(Texture *t, const float *xy, int n)
{
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            float px = x + 0.5f, py = y + 0.5f, best = 1e9f;
            int inside = 0;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                float xi = xy[i * 2], yi = xy[i * 2 + 1], xj = xy[j * 2], yj = xy[j * 2 + 1];
                if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) inside = !inside;
                float d = seg_dist(px, py, xi, yi, xj, yj);
                if (d < best) best = d;
            }
            put_alpha(t, x, y, inside ? 0.5f + best : 0.5f - best);
        }
}

void tex_stroke_round_rect(Texture *t, float x, float y, float w, float h, float r, float width)
{
    for (int py = 0; py < t->h; py++)
        for (int px = 0; px < t->w; px++) {
            float d = sd_round_box(px + 0.5f, py + 0.5f, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, r);
            put_alpha(t, px, py, width * 0.5f + 0.5f - fabsf(d));
        }
}

void gfx_init(void)
{
    for (int i = 0; i < 256; i++) alpha_clut[i] = 0x00FFFFFF | ((u32)i << 24);
    sceKernelDcacheWritebackRange(alpha_clut, sizeof(alpha_clut));

    void *fbp0 = guGetStaticVramBuffer(BUF_W, SCREEN_H, GU_PSM_8888);
    void *fbp1 = guGetStaticVramBuffer(BUF_W, SCREEN_H, GU_PSM_8888);

    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, fbp0, BUF_W);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, fbp1, BUF_W);
    sceGuOffset(2048 - SCREEN_W / 2, 2048 - SCREEN_H / 2);
    sceGuViewport(2048, 2048, SCREEN_W, SCREEN_H);
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void gfx_begin(u32 clear)
{
    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(clear);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    bound = NULL;
}

void gfx_end(void)
{
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void gfx_additive(int on)
{
    if (on) sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
    else sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

void gfx_rect(float x, float y, float w, float h, u32 color)
{
    CVert *v = sceGuGetMemory(2 * sizeof(CVert));
    v[0] = (CVert){ color, x, y, 0 };
    v[1] = (CVert){ color, x + w, y + h, 0 };
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, CVERT_FMT, 2, 0, v);
}

static void bind(const Texture *t)
{
    sceGuEnable(GU_TEXTURE_2D);
    if (bound == t) return;
    if (t->alpha8) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(256 / 8, alpha_clut);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    } else {
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    }
    sceGuTexImage(0, t->tw, t->th, t->tw, t->px);
    sceGuTexFlush();
    bound = t;
}

void gfx_gradient(float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br)
{
    CVert *v = sceGuGetMemory(4 * sizeof(CVert));
    v[0] = (CVert){ tl, x, y, 0 };
    v[1] = (CVert){ bl, x, y + h, 0 };
    v[2] = (CVert){ tr, x + w, y, 0 };
    v[3] = (CVert){ br, x + w, y + h, 0 };
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_TRIANGLE_STRIP, CVERT_FMT, 4, 0, v);
}

void gfx_clip(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    sceGuScissor(x, y, x + (w > 0 ? w : 0), y + (h > 0 ? h : 0));
}

void gfx_unclip(void)
{
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
}

void gfx_tex_part(const Texture *t, float sx, float sy, float sw, float sh,
                  float x, float y, float w, float h, u32 color)
{
    TVert *v = sceGuGetMemory(2 * sizeof(TVert));
    v[0] = (TVert){ sx, sy, color, x, y, 0 };
    v[1] = (TVert){ sx + sw, sy + sh, color, x + w, y + h, 0 };
    bind(t);
    sceGuDrawArray(GU_SPRITES, TVERT_FMT, 2, 0, v);
}

void gfx_filter(int linear)
{
    sceGuTexFilter(linear ? GU_LINEAR : GU_NEAREST, linear ? GU_LINEAR : GU_NEAREST);
}

void gfx_tex(const Texture *t, float x, float y, float w, float h, u32 color)
{
    gfx_tex_part(t, 0, 0, t->w, t->h, x, y, w, h, color);
}

void gfx_nine(const Texture *t, int b, float x, float y, float w, float h, u32 color)
{
    float sx[4] = { 0, b, t->w - b, t->w }, sy[4] = { 0, b, t->h - b, t->h };
    float dx[4] = { x, x + b, x + w - b, x + w }, dy[4] = { y, y + b, y + h - b, y + h };
    TVert *v = sceGuGetMemory(18 * sizeof(TVert));
    int n = 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            v[n++] = (TVert){ sx[c], sy[r], color, dx[c], dy[r], 0 };
            v[n++] = (TVert){ sx[c + 1], sy[r + 1], color, dx[c + 1], dy[r + 1], 0 };
        }
    bind(t);
    sceGuDrawArray(GU_SPRITES, TVERT_FMT, 18, 0, v);
}
