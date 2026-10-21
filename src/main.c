#include <pspctrl.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "flac.h"
#include "gfx.h"
#include "home.h"
#include "library.h"
#include "music.h"
#include "playlists.h"
#include "deck.h"
#include "pod.h"
#include "settings.h"
#include "stations.h"
#include "stats.h"
#include "video.h"
#include "visual.h"
#include "plib.h"
#include "pocket.h"
#include "splash.h"
#include "vlib.h"
#include "volume.h"
#include "player.h"
#include "sys.h"
#include "ui.h"

PSP_MODULE_INFO("Boro", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

enum { MODE_HOME, MODE_MUSIC, MODE_SETTINGS, MODE_STATS, MODE_VIDEO, MODE_SPLASH, MODE_NO_MUSIC };


// The music player in the theme picked in Settings.
static void music_enter(void)
{
    if (settings.theme == THEME_DECK) deck_enter();
    else if (settings.theme == THEME_POD) pod_enter();
    else flac_enter();
}

static int music_update(const Input *in, float dt)
{
    if (settings.theme == THEME_DECK) return deck_update(in, dt);
    if (settings.theme == THEME_POD) return pod_update(in, dt);
    return flac_update(in, dt);
}

static void music_draw(float dt)
{
    if (settings.theme == THEME_DECK) deck_draw(dt);
    else if (settings.theme == THEME_POD) pod_draw(dt);
    else flac_draw(dt);
}

int main(int argc, char *argv[])
{
#ifdef BORO_SCRIPT
    // The emulator has more memory than a PSP-3000; hold the extra back so
    // test runs run out where a real one would.
    {
        u32 lo = 0, hi = 128u << 20;
        while (hi - lo > (64 << 10)) { u32 m = (lo + hi) / 2; void *volatile b = malloc(m); if (b) { free(b); lo = m; } else hi = m; }
        static void *volatile ballast;
        if (lo > (46u << 20)) ballast = malloc(lo - (46u << 20));
    }
#endif
    // argv[0] is like "ms0:/PSP/GAME/Boro/EBOOT.PBP": keep data next to the
    // EBOOT and read music from MUSIC on the same device.
    char data_dir[256] = "ms0:/PSP/GAME/Boro/data", music_dir[64] = "ms0:/MUSIC", video_dir[64] = "ms0:/VIDEO", picture_dir[64] = "ms0:/PICTURE";
    if (argc > 0 && argv[0] && strchr(argv[0], ':')) {
        snprintf(data_dir, sizeof(data_dir), "%s", argv[0]);
        char *slash = strrchr(data_dir, '/');
        if (slash) strcpy(slash, "/data");
        int dev = strchr(argv[0], ':') - argv[0];
        snprintf(music_dir, sizeof(music_dir), "%.*s:/MUSIC", dev, argv[0]);
        snprintf(video_dir, sizeof(video_dir), "%.*s:/VIDEO", dev, argv[0]);
        snprintf(picture_dir, sizeof(picture_dir), "%.*s:/PICTURE", dev, argv[0]);
    }

    sys_init();
    settings_load(data_dir);
    gfx_init();
    ui_init();
    art_init();
    player_init();
    char app_dir[256];
    snprintf(app_dir, sizeof(app_dir), "%s", data_dir);
    *strrchr(app_dir, '/') = 0;
    volume_init(app_dir);
    library_start(data_dir, music_dir);
    vlib_init(data_dir, video_dir);
    plib_init(data_dir, picture_dir);
    home_init();

    u64 tick_res = sceRtcGetTickResolution(), last;
    sceRtcGetCurrentTick(&last);
    Input in;
    int mode = MODE_SPLASH, settings_from = MODE_HOME, last_song = -1, lists_loaded = 0, visual_on = 0, waking = 0;
    float idle_t = 0;
    int cpu_mhz = 0;
    while (sys_running()) {
        u64 now;
        sceRtcGetCurrentTick(&now);
        float dt = (float)(now - last) / tick_res;
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        sys_read_input(&in);
        int screen_off = pocket_frame(&in, dt);
        art_frame();
        volume_frame(dt);
        int song = player_song();
        if (song != last_song && song >= 0) library_note_played(song);
        last_song = song;
        if (!lists_loaded && library_state() == LIB_READY) {
            lists_loaded = 1;
            stations_load();
            playlists_load();
            stats_load(data_dir);
        }
        stats_frame(dt);
        settings_frame(dt);
        // While music plays the PSP mustn't go to sleep, and with "Keep
        // screen on" the backlight stays up too.
        if (player_song() >= 0 && !player_paused())
            scePowerTick(settings.keep_on && !screen_off ? PSP_POWER_TICK_ALL : PSP_POWER_TICK_SUSPEND);
        // Full speed only for video (decoding and thumbnails); menus and
        // music run slower to save battery.
        int want_mhz = mode == MODE_VIDEO ? 333 : 222;
        if (want_mhz != cpu_mhz) {
            scePowerSetClockFrequency(want_mhz, want_mhz, want_mhz / 2);
            cpu_mhz = want_mhz;
        }

        // Pocket mode: music plays on with the screen off. Black frames keep
        // it dark even if the backlight couldn't be switched off.
        if (screen_off) {
            gfx_begin(RGBA(0, 0, 0, 255));
            gfx_end();
            continue;
        }

        // Idle visualizer: any button (or the stick) wakes the screen, and
        // that press does nothing else.
        int touched = in.held || in.stick_x > 40 || in.stick_x < -40;
        idle_t = touched || pocket_locked() ? 0 : idle_t + dt; // unlocking returns to the same screen
        if (visual_on) {
            if (touched || player_song() < 0 || player_paused()) {
                visual_on = 0;
                idle_t = 0;
                waking = 1;
            } else {
                visual_draw(dt);
                continue;
            }
        }
        if (waking) {
            // Swallow the waking press until every button is up.
            if (touched) memset(&in, 0, sizeof(in));
            else waking = 0;
        }
        if (settings.idle_vis && idle_t > settings_idle_seconds() && player_song() >= 0 && !player_paused() && !video_playing()) {
            visual_on = 1;
            visual_enter();
            visual_draw(dt);
            continue;
        }

        if (mode == MODE_SPLASH) {
            if (splash_update(dt)) mode = MODE_HOME;
            if (mode == MODE_SPLASH) splash_draw();
            else home_draw();
        } else if (mode == MODE_NO_MUSIC) {
            if (in.pressed & PSP_CTRL_CIRCLE) mode = MODE_HOME;
            if (mode == MODE_NO_MUSIC) home_draw_no_music();
            else home_draw();
        } else if (mode == MODE_HOME) {
            int r = home_update(&in, dt);
            if (r == HOME_OPEN_MUSIC && library_state() == LIB_READY && !lib.song_count) {
                mode = MODE_NO_MUSIC;
            } else if (r == HOME_OPEN_MUSIC) {
                mode = MODE_MUSIC;
                music_enter();
            } else if (r == HOME_OPEN_VIDEO) {
                mode = MODE_VIDEO;
                video_enter();
            } else if (r == HOME_OPEN_STATS) {
                mode = MODE_STATS;
                stats_enter();
            } else if (r == HOME_OPEN_SETTINGS) {
                mode = MODE_SETTINGS;
                settings_from = MODE_HOME;
                settings_enter();
            }
            if (mode == MODE_HOME) home_draw();
            else if (mode == MODE_NO_MUSIC) home_draw_no_music();
        } else if (mode == MODE_VIDEO) {
            if (video_update(&in, dt)) mode = MODE_HOME;
            if (mode == MODE_VIDEO) video_draw(dt);
        } else if (mode == MODE_STATS) {
            if (stats_update(&in, dt)) mode = MODE_HOME;
            if (mode == MODE_STATS) stats_draw(dt);
        } else if (mode == MODE_SETTINGS) {
            if (settings_update(&in, dt)) {
                mode = settings_from;
                if (mode == MODE_MUSIC) music_enter();
            }
            if (mode == MODE_SETTINGS) settings_draw();
        } else {
            int r = music_update(&in, dt);
            if (r == MUSIC_HOME) mode = MODE_HOME;
            else if (r == MUSIC_SETTINGS) {
                mode = MODE_SETTINGS;
                settings_from = MODE_MUSIC;
                settings_enter();
            }
            if (mode == MODE_MUSIC) music_draw(dt);
        }
    }
    stats_flush();
    sceKernelExitGame();
    return 0;
}
