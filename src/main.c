#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <string.h>
#include "art.h"
#include "flac.h"
#include "gfx.h"
#include "home.h"
#include "library.h"
#include "player.h"
#include "sys.h"
#include "ui.h"

PSP_MODULE_INFO("Boro", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

enum { MODE_HOME, MODE_MUSIC };

int main(int argc, char *argv[])
{
    // argv[0] is like "ms0:/PSP/GAME/Boro/EBOOT.PBP": keep data next to the
    // EBOOT and read music from MUSIC on the same device.
    char data_dir[256] = "ms0:/PSP/GAME/Boro/data", music_dir[64] = "ms0:/MUSIC";
    if (argc > 0 && argv[0] && strchr(argv[0], ':')) {
        snprintf(data_dir, sizeof(data_dir), "%s", argv[0]);
        char *slash = strrchr(data_dir, '/');
        if (slash) strcpy(slash, "/data");
        int dev = strchr(argv[0], ':') - argv[0];
        snprintf(music_dir, sizeof(music_dir), "%.*s:/MUSIC", dev, argv[0]);
    }

    sys_init();
    gfx_init();
    ui_init();
    art_init();
    player_init();
    library_start(data_dir, music_dir);
    home_init();

    u64 tick_res = sceRtcGetTickResolution(), last;
    sceRtcGetCurrentTick(&last);
    Input in;
    int mode = MODE_HOME, last_song = -1;
    while (sys_running()) {
        u64 now;
        sceRtcGetCurrentTick(&now);
        float dt = (float)(now - last) / tick_res;
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        sys_read_input(&in);
        art_frame();
        int song = player_song();
        if (song != last_song && song >= 0) library_note_played(song);
        last_song = song;

        if (mode == MODE_HOME) {
            if (home_update(&in, dt) == HOME_OPEN_MUSIC) {
                mode = MODE_MUSIC;
                flac_enter();
            }
            home_draw();
        } else {
            if (flac_update(&in, dt)) mode = MODE_HOME;
            flac_draw(dt);
        }
    }
    sceKernelExitGame();
    return 0;
}
