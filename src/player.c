#include "player.h"

#include <pspaudio.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspmp3.h>
#include <psputility.h>
#include <stdlib.h>
#include <string.h>
#include "library.h"

#define RING 32768 // stereo frames buffered ahead (~0.7 s at 44.1 kHz)
#define BLOCK 1024 // frames per hardware output call
#define MARKS 16
#define DECODER_DELAY 529 // MP3 decoder latency in samples

typedef struct {
    u32 start; // ring position of the track's first frame
    int index; // position in order[]
    int rate;
} Marker;

static SceUID lock;
static s16 ring[RING * 2] __attribute__((aligned(64)));
static u32 wr, rd; // absolute frame counters
static Marker marks[MARKS];
static int mark_count;

static int *order, *list, count, list_count;
static int dec_index;            // order[] position the decoder works on
static volatile int flush_to = -1;
static volatile int cur_index = -1, cur_rate = 44100;
static volatile u32 cur_start;
static volatile int paused, shuffle, locked;

static void take(void) { sceKernelWaitSema(lock, 1, NULL); }
static void give(void) { sceKernelSignalSema(lock, 1); }

// ---- Decoder --------------------------------------------------------------

static SceUChar8 mp3_buf[16 * 1024] __attribute__((aligned(64)));
static SceShort16 pcm_buf[16 * 1152] __attribute__((aligned(64)));

typedef struct {
    SceUID fd;
    int handle;
    u32 skip, remaining; // gapless trimming, in frames
    int rate;
} Track;

static int feed(Track *t)
{
    SceUChar8 *dst;
    SceInt32 want, pos;
    if (sceMp3GetInfoToAddStreamData(t->handle, &dst, &want, &pos) < 0) return -1;
    if (sceIoLseek32(t->fd, pos, PSP_SEEK_SET) < 0) return -1;
    int got = sceIoRead(t->fd, dst, want);
    if (got < 0) return -1;
    sceKernelDcacheWritebackRange(dst, got); // the Media Engine reads it from memory
    return sceMp3NotifyAddStreamData(t->handle, got) < 0 ? -1 : got;
}

static void close_track(Track *t)
{
    if (t->handle >= 0) sceMp3ReleaseMp3Handle(t->handle);
    if (t->fd >= 0) sceIoClose(t->fd);
    t->handle = t->fd = -1;
}

static int open_track(Track *t, int song)
{
    const Song *s = &lib.songs[song];
    t->handle = -1;
    t->fd = sceIoOpen(song_path(s), PSP_O_RDONLY, 0);
    if (t->fd < 0) return -1;
    SceMp3InitArg init = {
        .mp3StreamStart = s->audio_start,
        .mp3StreamEnd = s->audio_end,
        .mp3Buf = mp3_buf,
        .mp3BufSize = sizeof(mp3_buf),
        .pcmBuf = (SceUChar8 *)pcm_buf,
        .pcmBufSize = sizeof(pcm_buf),
    };
    t->handle = sceMp3ReserveMp3Handle(&init);
    if (t->handle < 0 || feed(t) < 0 || sceMp3Init(t->handle) < 0) {
        close_track(t);
        return -1;
    }
    sceMp3SetLoopNum(t->handle, 0);
    t->rate = sceMp3GetSamplingRate(t->handle);
    t->skip = s->enc_delay >= 0 ? s->enc_delay + DECODER_DELAY : 0;
    t->remaining = s->total_samples ? s->total_samples : 0xFFFFFFFF;
    return 0;
}

// Decodes one MP3 frame into the ring. Returns 0 at the end of the track.
static int decode_frame(Track *t)
{
    if (sceMp3CheckStreamDataNeeded(t->handle) > 0 && feed(t) < 0) return 0;
    SceShort16 *pcm;
    int bytes = sceMp3Decode(t->handle, &pcm);
    if (bytes <= 0) return 0;
    // The Media Engine writes the PCM straight to memory: drop any stale
    // cached copy before the CPU reads it.
    sceKernelDcacheInvalidateRange(pcm_buf, sizeof(pcm_buf));
    u32 frames = bytes / 4; // the decoder always outputs 16-bit stereo
    if (t->skip) {
        u32 n = t->skip < frames ? t->skip : frames;
        t->skip -= n;
        pcm += n * 2;
        frames -= n;
    }
    if (frames > t->remaining) frames = t->remaining;
    t->remaining -= frames;

    take();
    for (u32 i = 0; i < frames; i++) {
        u32 at = (wr + i) % RING;
        ring[at * 2] = pcm[i * 2];
        ring[at * 2 + 1] = pcm[i * 2 + 1];
    }
    wr += frames;
    give();
    return t->remaining > 0;
}

static void shuffle_from(int first);

// Appends a fresh shuffle of the list to the play order. Caller holds the lock.
static void extend_radio(void)
{
    int *grown = realloc(order, (count + list_count) * sizeof(int));
    if (!grown) return;
    order = grown;
    int last = count ? order[count - 1] : -1, first = count;
    memcpy(order + count, list, list_count * sizeof(int));
    count += list_count;
    shuffle_from(first - 1 < 0 ? 0 : first - 1);
    // Don't repeat the song that just played.
    if (list_count > 1 && order[first] == last) {
        int tmp = order[first];
        order[first] = order[first + 1];
        order[first + 1] = tmp;
    }
}

static int decoder_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    Track t = { -1, -1, 0, 0, 0 };
    int open = 0;
    for (;;) {
        if (flush_to >= 0) {
            if (open) close_track(&t);
            open = 0;
            take();
            dec_index = flush_to;
            rd = wr = 0;
            mark_count = 0;
            cur_index = flush_to < count ? flush_to : -1;
            cur_start = 0;
            flush_to = -1;
            give();
        }
        if (!open) {
            take();
            int index = dec_index, song = index < count ? order[index] : -1;
            // Radio never runs out: another shuffled pass follows the last song.
            if (song < 0 && locked && list_count && index == count) {
                extend_radio();
                give();
                continue;
            }
            give();
            if (song < 0) {
                sceKernelDelayThread(10000);
                continue;
            }
            if (open_track(&t, song) < 0) {
                take();
                if (flush_to < 0) dec_index++;
                give();
                continue;
            }
            open = 1;
            take();
            if (mark_count == MARKS) memmove(marks, marks + 1, --mark_count * sizeof(Marker));
            marks[mark_count++] = (Marker){ wr, index, t.rate };
            give();
        }
        take();
        u32 used = wr - rd;
        give();
        if (used > RING - 2 * 1152) {
            sceKernelDelayThread(5000);
            continue;
        }
        if (!decode_frame(&t)) {
            close_track(&t);
            open = 0;
            take();
            if (flush_to < 0) dec_index++;
            give();
        }
    }
    return 0;
}

// ---- Output -----------------------------------------------------------------

// The hardware keeps reading a block after the output call returns, so two
// blocks alternate: the next one is filled while the previous one plays.
static s16 out_bufs[2][BLOCK * 2] __attribute__((aligned(64)));

static int output_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int channel_rate = 0, which = 0;
    for (;;) {
        if (paused) {
            sceKernelDelayThread(10000);
            continue;
        }
        take();
        // Tracks become audible when the read position reaches their marker.
        while (mark_count && marks[0].start <= rd) {
            cur_index = marks[0].index;
            cur_start = marks[0].start;
            cur_rate = marks[0].rate;
            memmove(marks, marks + 1, --mark_count * sizeof(Marker));
        }
        u32 avail = wr - rd, n = avail < BLOCK ? avail : BLOCK;
        int finished = dec_index >= count;
        // Never mix two sample rates in one block.
        if (mark_count && marks[0].start < rd + n && marks[0].rate != cur_rate) n = marks[0].start - rd;
        else if (n < BLOCK && !finished) n = 0; // wait for a full block
        if (n == 0) {
            if (finished && !mark_count && avail == 0 && cur_index >= 0 && flush_to < 0) cur_index = -1;
            give();
            sceKernelDelayThread(3000);
            continue;
        }
        s16 *out_buf = out_bufs[which];
        which ^= 1;
        for (u32 i = 0; i < n; i++) {
            u32 at = (rd + i) % RING;
            out_buf[i * 2] = ring[at * 2];
            out_buf[i * 2 + 1] = ring[at * 2 + 1];
        }
        memset(out_buf + n * 2, 0, (BLOCK - n) * 4);
        rd += n;
        int rate = cur_rate;
        give();
        if (rate != channel_rate) {
            if (channel_rate) sceAudioSRCChRelease();
            sceAudioSRCChReserve(BLOCK, rate, 2);
            channel_rate = rate;
        }
        sceKernelDcacheWritebackRange(out_buf, sizeof(out_bufs[0]));
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out_buf);
    }
    return 0;
}

// ---- Control ----------------------------------------------------------------

void player_init(void)
{
    sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    sceUtilityLoadModule(PSP_MODULE_AV_MP3);
    sceMp3InitResource();
    lock = sceKernelCreateSema("player", 0, 1, 1, NULL);
    SceUID out = sceKernelCreateThread("boro_audio_out", output_thread, 0x12, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    SceUID dec = sceKernelCreateThread("boro_audio_dec", decoder_thread, 0x14, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (out >= 0) sceKernelStartThread(out, 0, NULL);
    if (dec >= 0) sceKernelStartThread(dec, 0, NULL);
}

static void shuffle_from(int first)
{
    for (int i = count - 1; i > first; i--) {
        int j = first + 1 + rand() % (i - first);
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

void player_play(const int *songs, int n, int start, int shuf)
{
    if (n <= 0) return;
    take();
    free(order);
    free(list);
    order = malloc(n * sizeof(int));
    list = malloc(n * sizeof(int));
    memcpy(list, songs, n * sizeof(int));
    memcpy(order, songs, n * sizeof(int));
    count = list_count = n;
    shuffle = shuf;
    locked = 0;
    if (start < 0 || start >= n) start = shuf ? rand() % n : 0;
    if (shuf) {
        order[0] = songs[start];
        order[start] = songs[0];
        shuffle_from(0);
        start = 0;
    }
    paused = 0;
    flush_to = start;
    cur_index = start;
    give();
}

void player_toggle_pause(void)
{
    if (cur_index >= 0) paused = !paused;
}

static void jump(int index)
{
    take();
    if (index < 0) index = 0;
    paused = 0;
    flush_to = index;
    cur_index = index < count ? index : -1;
    give();
}

void player_next(void)
{
    if (locked || cur_index < 0) return;
    jump(cur_index + 1);
}

void player_prev(void)
{
    if (locked || cur_index < 0) return;
    jump(player_position_ms() > 3000 ? cur_index : cur_index - 1);
}

void player_set_shuffle(int on)
{
    take();
    if (on == shuffle || !count || locked) {
        shuffle = on;
        give();
        return;
    }
    shuffle = on;
    int song = cur_index >= 0 ? order[cur_index] : -1, at = 0;
    if (on) {
        memcpy(order, list, count * sizeof(int));
        for (int i = 0; i < count; i++)
            if (order[i] == song) { order[i] = order[0]; order[0] = song; }
        shuffle_from(0);
    } else {
        memcpy(order, list, count * sizeof(int));
        for (int i = 0; i < count; i++)
            if (order[i] == song) at = i;
    }
    // Keep the current song playing; only what comes after it changes. If the
    // decoder already moved on to the next song, restart it from the new one.
    int ahead = dec_index != cur_index;
    if (cur_index >= 0) {
        for (int m = 0; m < mark_count; m++) marks[m].index = at;
        cur_index = at;
        if (ahead) flush_to = at + 1;
        else dec_index = at;
    }
    give();
}

void player_set_locked(int on) { locked = on; }

void player_jump(int index)
{
    if (locked || index < 0 || index >= count) return;
    jump(index);
}

int player_queue(int *out, int max, int *first_index)
{
    take();
    int first = cur_index + 1, n = 0;
    for (int i = first; i < count && n < max; i++) out[n++] = order[i];
    give();
    *first_index = first;
    return n;
}
int player_song(void)
{
    int i = cur_index;
    return i >= 0 && i < count ? order[i] : -1;
}
int player_paused(void) { return paused; }
int player_shuffle(void) { return shuffle; }
int player_locked(void) { return locked; }

u32 player_position_ms(void)
{
    if (cur_index < 0 || flush_to >= 0) return 0;
    u32 played = rd - cur_start;
    return (u32)((u64)played * 1000 / (cur_rate ? cur_rate : 44100));
}
