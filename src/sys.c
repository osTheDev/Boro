#include "sys.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <stdio.h>
#include <string.h>
#include <pspiofilemgr.h>

static volatile int running = 1;
static u32 last_buttons, repeat_mask, repeat_start, repeat_next;

#define NAV_BUTTONS (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER)

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    running = 0;
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int cb = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

void sys_init(void)
{
    int th = sceKernelCreateThread("boro_callbacks", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

int sys_running(void)
{
    return running;
}

void sys_quit(void)
{
    running = 0;
}

#ifdef BORO_SCRIPT
// Test builds replay button presses from ms0:/boro_script.txt, one
// "<milliseconds> <button> [hold milliseconds]" per line, so the emulator can
// reach any screen.
static struct { u32 at, until; u32 button; } script[64];
static int script_len, script_pos;
static u32 script_start;

static void load_script(void)
{
    static const struct { const char *name; u32 bit; } names[] = {
        { "UP", PSP_CTRL_UP }, { "DOWN", PSP_CTRL_DOWN }, { "LEFT", PSP_CTRL_LEFT }, { "RIGHT", PSP_CTRL_RIGHT },
        { "CROSS", PSP_CTRL_CROSS }, { "CIRCLE", PSP_CTRL_CIRCLE }, { "SQUARE", PSP_CTRL_SQUARE },
        { "TRIANGLE", PSP_CTRL_TRIANGLE }, { "L", PSP_CTRL_LTRIGGER }, { "R", PSP_CTRL_RTRIGGER },
        { "START", PSP_CTRL_START }, { "SELECT", PSP_CTRL_SELECT },
    };
    char buf[2048];
    SceUID fd = sceIoOpen("ms0:/boro_script.txt", PSP_O_RDONLY, 0);
    if (fd < 0) return;
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    for (char *line = strtok(buf, "\n"); line && script_len < 64; line = strtok(NULL, "\n")) {
        char name[16];
        unsigned at, hold = 0;
        if (sscanf(line, "%u %15s %u", &at, name, &hold) < 2) continue;
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(names[i].name, name)) script[script_len++] = (typeof(script[0])){ at * 1000, (at + hold) * 1000, names[i].bit };
    }
    script_start = sceKernelGetSystemTimeLow();
}
#endif

void sys_read_input(Input *in)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
#ifdef BORO_SCRIPT
    static int loaded;
    if (!loaded) { loaded = 1; load_script(); }
    u32 t = sceKernelGetSystemTimeLow() - script_start;
    if (script_pos < script_len && t >= script[script_pos].at)
        pad.Buttons |= script[script_pos++].button;
    for (int i = 0; i < script_pos; i++)
        if (t < script[i].until) pad.Buttons |= script[i].button;
#endif
    // The analog stick acts as the d-pad. A direction engages past 70% of the
    // throw and lets go under 45%, so a stick resting near center never drifts.
    static u32 stick;
    int dx = pad.Lx - 128, dy = pad.Ly - 128;
    struct { int v; u32 bit; } dirs[] = {
        { -dx, PSP_CTRL_LEFT }, { dx, PSP_CTRL_RIGHT }, { -dy, PSP_CTRL_UP }, { dy, PSP_CTRL_DOWN },
    };
    for (int i = 0; i < 4; i++) {
        if (dirs[i].v > 90) stick |= dirs[i].bit;
        else if (dirs[i].v < 58) stick &= ~dirs[i].bit;
    }
    pad.Buttons |= stick;
    in->stick_x = dx;
    in->held = pad.Buttons;
    in->pressed = pad.Buttons & ~last_buttons;
    in->nav = in->pressed;
    last_buttons = pad.Buttons;

    // Held d-pad / shoulder buttons repeat after 350 ms, speeding up over time.
    u32 now = sceKernelGetSystemTimeLow(), nav = pad.Buttons & NAV_BUTTONS;
    if (in->pressed & NAV_BUTTONS) {
        repeat_mask = in->pressed & NAV_BUTTONS;
        repeat_start = now;
        repeat_next = now + 350000;
    } else if (!(nav & repeat_mask)) {
        repeat_mask = 0;
    } else if ((s32)(now - repeat_next) >= 0) {
        in->nav |= nav & repeat_mask;
        u32 held_for = now - repeat_start;
        repeat_next = now + (held_for > 2000000 ? 25000 : held_for > 1000000 ? 45000 : 80000);
    }
}

void sys_clock(char *out, int size)
{
    ScePspDateTime t;
    if (sceRtcGetCurrentClockLocalTime(&t) < 0) {
        snprintf(out, size, "--:--");
        return;
    }
    int h = t.hour % 12;
    snprintf(out, size, "%d:%02d %s", h ? h : 12, t.minute, t.hour < 12 ? "AM" : "PM");
}

int sys_battery(int *charging)
{
    *charging = scePowerIsBatteryCharging() == 1;
    if (scePowerIsBatteryExist() != 1) return -1;
    return scePowerGetBatteryLifePercent();
}
