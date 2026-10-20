#include "scrub.h"

#include <math.h>
#include "library.h"
#include "player.h"

static int scrubbing; // 1 while the stick is pushed, 2 while the seek lands
static float scrub_ms, settle_t;

void scrub_update(const Input *in, float dt)
{
    // Further out moves faster, up to 40 seconds a second.
    int song = player_song(), dx = in->stick_x, dead = 40;
    if (song >= 0 && !player_locked() && (dx > dead || dx < -dead)) {
        if (scrubbing != 1) { scrub_ms = scrubbing ? scrub_ms : player_position_ms(); scrubbing = 1; }
        float k = (fabsf((float)dx) - dead) / (128 - dead);
        scrub_ms += (dx > 0 ? 1 : -1) * k * k * 40000 * dt;
        float dur = lib.songs[song].duration_ms;
        if (scrub_ms < 0) scrub_ms = 0;
        if (scrub_ms > dur - 1000) scrub_ms = dur > 1000 ? dur - 1000 : 0;
    } else if (scrubbing == 1) {
        // Hold the bar at the target until the new position comes through.
        scrubbing = 2;
        settle_t = 0.6f;
        if (song >= 0) player_seek((u32)scrub_ms);
    } else if (scrubbing == 2) {
        settle_t -= dt;
        float off = (float)player_position_ms() - scrub_ms;
        if (settle_t <= 0 || (off > -1500 && off < 1500)) scrubbing = 0;
    }
}

u32 scrub_position(void) { return scrubbing ? (u32)scrub_ms : player_position_ms(); }
int scrub_active(void) { return scrubbing != 0; }
