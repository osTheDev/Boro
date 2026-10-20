#include "vplayer.h"

#include <malloc.h>
#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avc.h"

// Decoded frames wait in slots until they are due. The slot on screen is
// only reused once a newer one replaces it.
#define SLOTS 4
enum { SLOT_FREE, SLOT_READY, SLOT_SHOWN };
typedef struct {
    Texture *tex;
    int state, gen;
    u32 pts; // ms
} Slot;

#define OUT_BLOCK 1024 // frames per audio output call, at 44.1 kHz
#define OUT_RATE 44100

static Mp4 mp4;
static int open_;
static char path_[256];
static SceUID video_th = -1, audio_th = -1, lock = -1;
static volatile int quit, paused, gain = 100;
// Each seek bumps gen; the threads restart from target_ms when they see it.
static volatile int gen;
static volatile u32 target_ms;
static volatile int ready_gen = -1;      // the video has its first frame for this gen
static volatile int video_done, audio_done, video_ok;
static volatile u32 clock_ms;            // what the speaker is playing now
static Slot slots[SLOTS];
static int shown = -1;

static void take(void) { sceKernelWaitSema(lock, 1, NULL); }
static void give(void) { sceKernelSignalSema(lock, 1); }

static int read_at(SceUID fd, u32 offset, void *buf, u32 size)
{
    if (sceIoLseek32(fd, offset, PSP_SEEK_SET) != (int)offset) return -1;
    return sceIoRead(fd, buf, size) == (int)size ? 0 : -1;
}

static u32 max_size(const Mp4Track *t)
{
    u32 m = 0;
    for (int i = 0; i < t->count; i++) if (t->size[i] > m) m = t->size[i];
    return m;
}

// ---- Video ------------------------------------------------------------------

// Presentation times of samples sent to the decoder but not out yet, sorted.
#define PENDING 32
typedef struct {
    u32 pts[PENDING];
    int n, gen;
    u32 skip_until; // frames before this are decoded but dropped (after a seek)
} Pending;

static u32 sample_pts(int i)
{
    const Mp4Track *t = &mp4.video;
    s64 v = t->dts[i];
    if (t->cts) v += t->cts[i] - t->cts[0];
    return mp4_ms(t, v < 0 ? 0 : (u32)v);
}

static void pending_add(Pending *p, u32 pts)
{
    if (p->n == PENDING) memmove(p->pts, p->pts + 1, --p->n * sizeof(u32));
    int i = p->n++;
    while (i > 0 && p->pts[i - 1] > pts) { p->pts[i] = p->pts[i - 1]; i--; }
    p->pts[i] = pts;
}

static void on_frame(const u32 *px, int w, int h, void *ctx)
{
    Pending *p = ctx;
    u32 pts = 0;
    if (p->n) { pts = p->pts[0]; memmove(p->pts, p->pts + 1, --p->n * sizeof(u32)); }
    if (pts < p->skip_until) return;
    // Wait for a free slot.
    int s = -1;
    while (!quit && p->gen == gen) {
        take();
        for (int i = 0; i < SLOTS && s < 0; i++) if (slots[i].state == SLOT_FREE) s = i;
        give();
        if (s >= 0) break;
        sceKernelDelayThread(4000);
    }
    if (s < 0) return;
    Texture *t = slots[s].tex;
    // The hardware leaves alpha at zero; the screen needs it opaque.
    for (int y = 0; y < h; y++) {
        const u32 *src = px + y * AVC_STRIDE;
        u32 *dst = t->px + y * t->tw;
        for (int x = 0; x < w; x++) dst[x] = src[x] | 0xFF000000;
    }
    t->w = w;
    t->h = h;
    tex_upload_rows(t, 0, h);
    take();
    slots[s].pts = pts;
    slots[s].gen = p->gen;
    slots[s].state = SLOT_READY;
    give();
    ready_gen = p->gen;
}

static int video_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    SceUID fd = sceIoOpen(path_, PSP_O_RDONLY, 0);
    u32 cap = max_size(&mp4.video);
    u8 *buf = memalign(64, cap + 64);
    Pending p = { .gen = -1 };
    int sample = 0, first = 1, failures = 0;
    while (!quit) {
        if (p.gen != gen) {
            p.gen = gen;
            p.n = 0;
            p.skip_until = target_ms;
            sample = mp4_keyframe_before(&mp4, mp4_sample_at(&mp4.video, target_ms));
            video_done = 0;
            first = 1;
            if (video_ok && avc_open(&mp4) != 0) video_ok = 0;
        }
        if (!video_ok || fd < 0 || !buf) { video_done = 1; ready_gen = gen; sceKernelDelayThread(20000); continue; }
        if (sample >= mp4.video.count) {
            video_done = 1;
            ready_gen = gen;
            sceKernelDelayThread(10000);
            continue;
        }
        u32 size = mp4.video.size[sample];
        if (read_at(fd, mp4.video.offset[sample], buf, size) != 0) { sample++; continue; }
        pending_add(&p, sample_pts(sample));
        int r = avc_decode(buf, size, first, 1, on_frame, &p);
        first = 0;
        sample++;
        // A decoder that never produces a picture means no video on this
        // system; keep the sound going.
        if (r > 0) failures = 0;
        else if (++failures > 60 && ready_gen != gen) video_ok = 0;
    }
    avc_close();
    free(buf);
    if (fd >= 0) sceIoClose(fd);
    return 0;
}

// ---- Audio ------------------------------------------------------------------

static unsigned long codec[65] __attribute__((aligned(64)));
static s16 pcm[1024 * 2] __attribute__((aligned(64)));
static s16 out_bufs[2][OUT_BLOCK * 2] __attribute__((aligned(64)));

static int audio_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    SceUID fd = sceIoOpen(path_, PSP_O_RDONLY, 0);
    const Mp4Track *t = &mp4.audio;
    int ok = fd >= 0 && mp4.has_audio && mp4.sample_rate > 0;
    void *work = NULL;
    if (ok) {
        memset(codec, 0, sizeof(codec));
        ok = sceAudiocodecCheckNeedMem(codec, PSP_CODEC_AAC) >= 0 && codec[4] &&
             (work = memalign(64, codec[4])) != NULL;
        if (ok) {
            codec[3] = (unsigned long)work;
            codec[10] = mp4.sample_rate;
            ok = sceAudiocodecInit(codec, PSP_CODEC_AAC) >= 0;
        }
    }
    u32 cap = ok ? max_size(t) : 0;
    u8 *src = ok ? memalign(64, cap + 64) : NULL;
    int channel = ok && src ? sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, OUT_BLOCK, PSP_AUDIO_FORMAT_STEREO) : -1;
    if (channel < 0) ok = 0;

    int my_gen = -1, sample = 0, which = 0, out_n = 0;
    // Resampler: pos is the read position in the decoded block, 16.16 fixed.
    s32 step = ok ? (s32)(((u64)mp4.sample_rate << 16) / OUT_RATE) : 0, pos = 0;
    int have = 0;      // frames in pcm
    s16 last[2] = { 0, 0 };
    u64 base_ms = 0, played = 0; // played: output frames since base_ms
    u32 last_tick = sceKernelGetSystemTimeLow();
    while (!quit) {
        if (my_gen != gen) {
            my_gen = gen;
            audio_done = 0;
            base_ms = target_ms;
            played = 0;
            clock_ms = target_ms;
            if (ok) {
                sample = mp4_sample_at(t, target_ms);
                base_ms = mp4_ms(t, t->dts[sample]);
                clock_ms = base_ms;
            }
            have = 0;
            pos = 0;
            out_n = 0;
            // Start once the first picture is ready, so they start together.
            for (int i = 0; i < 300 && !quit && my_gen == gen && ready_gen != gen; i++) sceKernelDelayThread(10000);
            last_tick = sceKernelGetSystemTimeLow();
        }
        if (paused) { sceKernelDelayThread(10000); last_tick = sceKernelGetSystemTimeLow(); continue; }
        if (!ok) {
            // No sound: the system clock runs the video.
            sceKernelDelayThread(10000);
            u32 now = sceKernelGetSystemTimeLow();
            played += (u64)(now - last_tick) * OUT_RATE / 1000000;
            last_tick = now;
            clock_ms = base_ms + played * 1000 / OUT_RATE;
            if (clock_ms >= mp4.duration_ms) audio_done = 1;
            continue;
        }
        // Fill one output block, decoding AAC frames as needed.
        s16 *out = out_bufs[which];
        while (out_n < OUT_BLOCK) {
            if ((pos >> 16) + 1 >= have) {
                if (have) { last[0] = pcm[(have - 1) * 2]; last[1] = pcm[(have - 1) * 2 + 1]; }
                pos -= have << 16;
                have = 0;
                if (sample >= t->count) break;
                u32 size = t->size[sample];
                if (read_at(fd, t->offset[sample], src, size) == 0) {
                    codec[6] = (unsigned long)src;
                    codec[7] = size;
                    codec[8] = (unsigned long)pcm;
                    codec[9] = sizeof(pcm);
                    if (sceAudiocodecDecode(codec, PSP_CODEC_AAC) >= 0) have = 1024;
                }
                sample++;
                if (!have) continue;
                // pos is now -1..0 frames into this block; frame -1 is `last`.
            }
            int i = pos >> 16;
            u32 f = pos & 0xFFFF;
            for (int c = 0; c < 2; c++) {
                int a = i < 0 ? last[c] : pcm[i * 2 + c];
                int b = pcm[(i + 1) * 2 + c];
                out[out_n * 2 + c] = (s16)(a + (((b - a) * (int)f) >> 16));
            }
            out_n++;
            pos += step;
        }
        if (out_n == 0) { audio_done = 1; sceKernelDelayThread(10000); continue; }
        memset(out + out_n * 2, 0, (OUT_BLOCK - out_n) * 4);
        int vol = PSP_AUDIO_VOLUME_MAX * gain / 100;
        sceAudioOutputPannedBlocking(channel, vol, vol, out);
        // The block just queued plays after the one before it, so the clock
        // trails what has been handed over by one block.
        played += out_n;
        u64 heard = played > OUT_BLOCK ? played - OUT_BLOCK : 0;
        if (my_gen == gen) clock_ms = base_ms + heard * 1000 / OUT_RATE;
        which ^= 1;
        out_n = 0;
    }
    if (channel >= 0) sceAudioChRelease(channel);
    free(src);
    free(work);
    if (fd >= 0) sceIoClose(fd);
    return 0;
}

// ---- Control ----------------------------------------------------------------

int vplayer_open(const char *path, u32 start_ms)
{
    vplayer_close();
    int r = mp4_open(&mp4, path);
    if (r != 0) return r;
    if (lock < 0) lock = sceKernelCreateSema("vplayer", 0, 1, 1, NULL);
    for (int i = 0; i < SLOTS; i++) {
        if (!slots[i].tex) slots[i].tex = tex_create(480, 272);
        slots[i].state = SLOT_FREE;
    }
    shown = -1;
    snprintf(path_, sizeof(path_), "%s", path);
    quit = paused = 0;
    video_ok = avc_available();
    video_done = audio_done = 0;
    ready_gen = -1;
    target_ms = start_ms < mp4.duration_ms ? start_ms : 0;
    clock_ms = target_ms;
    gen++;
    open_ = 1;
    video_th = sceKernelCreateThread("boro_video", video_thread, 0x16, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    audio_th = sceKernelCreateThread("boro_vaudio", audio_thread, 0x11, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (video_th >= 0) sceKernelStartThread(video_th, 0, NULL);
    if (audio_th >= 0) sceKernelStartThread(audio_th, 0, NULL);
    return 0;
}

void vplayer_close(void)
{
    if (!open_) return;
    quit = 1;
    SceUInt timeout = 2000000;
    if (video_th >= 0) { sceKernelWaitThreadEnd(video_th, &timeout); sceKernelDeleteThread(video_th); }
    timeout = 2000000;
    if (audio_th >= 0) { sceKernelWaitThreadEnd(audio_th, &timeout); sceKernelDeleteThread(audio_th); }
    video_th = audio_th = -1;
    mp4_close(&mp4);
    open_ = 0;
}

const Mp4 *vplayer_mp4(void) { return open_ ? &mp4 : NULL; }
void vplayer_toggle_pause(void) { paused = !paused; }
int vplayer_paused(void) { return paused; }
u32 vplayer_position(void) { return clock_ms < mp4.duration_ms ? clock_ms : mp4.duration_ms; }
int vplayer_video_ok(void) { return video_ok; }
void vplayer_set_gain(int percent) { gain = percent < 0 ? 0 : percent > 100 ? 100 : percent; }

void vplayer_seek(u32 ms)
{
    if (!open_) return;
    if (ms >= mp4.duration_ms) ms = mp4.duration_ms > 1000 ? mp4.duration_ms - 1000 : 0;
    target_ms = ms;
    clock_ms = ms;
    gen++;
}

int vplayer_ended(void)
{
    if (!open_) return 1;
    if (!audio_done || !video_done) return 0;
    take();
    int waiting = 0;
    for (int i = 0; i < SLOTS; i++) if (slots[i].state == SLOT_READY && slots[i].gen == gen) waiting = 1;
    give();
    return !waiting;
}

const Texture *vplayer_frame(void)
{
    if (!open_) return NULL;
    take();
    u32 now = clock_ms;
    int best = -1;
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &slots[i];
        if (s->state != SLOT_READY) continue;
        if (s->gen != gen) { s->state = SLOT_FREE; continue; }
        if (s->pts <= now + 10 && (best < 0 || s->pts > slots[best].pts)) best = i;
    }
    // Before the clock starts after a seek, show the first frame right away.
    if (best < 0 && (shown < 0 || slots[shown].gen != gen)) {
        for (int i = 0; i < SLOTS; i++)
            if (slots[i].state == SLOT_READY && (best < 0 || slots[i].pts < slots[best].pts)) best = i;
    }
    if (best >= 0) {
        // Frames older than the one going up are late: drop them.
        for (int i = 0; i < SLOTS; i++)
            if (i != best && slots[i].state == SLOT_READY && slots[i].pts < slots[best].pts) slots[i].state = SLOT_FREE;
        if (shown >= 0) slots[shown].state = SLOT_FREE;
        slots[best].state = SLOT_SHOWN;
        shown = best;
    }
    give();
    return shown >= 0 ? slots[shown].tex : NULL;
}
