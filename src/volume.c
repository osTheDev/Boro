#include "volume.h"

#include <kubridge.h>
#include <pspkernel.h>
#include <stdio.h>

#define SHOW_SECONDS 1.6f
#define FADE_SECONDS 0.25f

int boroGetVolume(void); // exported by boro_kernel.prx

static int loaded, level = -1;
static float show_t;

void volume_init(const char *app_dir)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/boro_kernel.prx", app_dir);
    SceUID mod = kuKernelLoadModule(path, 0, NULL);
    if (mod < 0) mod = sceKernelLoadModule(path, 0, NULL);
    if (mod <= 0 || sceKernelStartModule(mod, 0, NULL, NULL, NULL) < 0) return;
    int v = boroGetVolume();
    if (v < 0 || v > VOLUME_MAX) return;
    loaded = 1;
    level = v;
}

void volume_frame(float dt)
{
    if (show_t > 0) show_t -= dt;
    if (!loaded) return;
    int v = boroGetVolume();
    if (v < 0 || v > VOLUME_MAX || v == level) return;
    level = v;
    show_t = SHOW_SECONDS;
}

int volume_level(void) { return level; }

float volume_shown(void)
{
    if (show_t <= 0) return 0;
    float in = (SHOW_SECONDS - show_t) / 0.12f, out = show_t / FADE_SECONDS;
    float a = in < out ? in : out;
    return a > 1 ? 1 : a;
}
