#include "sys.h"

#include <pspctrl.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <stdio.h>

static volatile int running = 1;
static u32 last_buttons;

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
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
}

int sys_running(void)
{
    return running;
}

void sys_quit(void)
{
    running = 0;
}

void sys_read_input(Input *in)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    in->held = pad.Buttons;
    in->pressed = pad.Buttons & ~last_buttons;
    last_buttons = pad.Buttons;
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
