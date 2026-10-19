#include <pspkernel.h>
#include <psprtc.h>
#include "font.h"
#include "gfx.h"
#include "home.h"
#include "sys.h"

PSP_MODULE_INFO("Boro", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

int main(void)
{
    sys_init();
    gfx_init();
    font_init();
    home_init();
    font_finish();

    u64 tick_res = sceRtcGetTickResolution(), last;
    sceRtcGetCurrentTick(&last);
    Input in;
    while (sys_running()) {
        u64 now;
        sceRtcGetCurrentTick(&now);
        float dt = (float)(now - last) / tick_res;
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        sys_read_input(&in);
        home_update(&in, dt);
        home_draw();
    }
    sceKernelExitGame();
    return 0;
}
