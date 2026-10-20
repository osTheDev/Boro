#include "avc.h"

#include <kubridge.h>
#include <malloc.h>
#include <pspkernel.h>
#include <pspmpeg.h>
#include <psputility.h>
#include <stdlib.h>
#include <string.h>

// mpeg_vsh.prx functions the PSPSDK doesn't declare (stubs in mpegavc.S).
typedef struct {
    void *sps; int sps_size;
    void *pps; int pps_size;
    int nal_prefix_size;
    const void *nal; int nal_size;
    int mode; // 3 for the first access unit, then 0
} AvcNal;
typedef struct { int unk0, unk1, width, height, unk[6]; } AvcInfo;
typedef struct { void *buffer[8]; int unk[3]; } AvcYuv;
typedef struct { int unk0[4]; AvcInfo *info; int unk1[6]; AvcYuv *yuv; int unk2[12]; } AvcDetail2;
typedef struct { int height, width, mode0, mode1; void *buffer[8]; } AvcCsc;
int sceMpegGetAvcNalAu(SceMpeg *mpeg, AvcNal *nal, SceMpegAu *au);
int sceMpegAvcDecodeDetail2(SceMpeg *mpeg, AvcDetail2 **detail);
int sceMpegBaseCscAvc(void *dest, int unk, int width, AvcCsc *csc);
int boroMeBoot(int type); // boro_kernel.prx

static int modules = -1; // -1 untried, 0 failed, 1 loaded
static int inited, created;
static SceMpeg mpeg;
static SceMpegRingbuffer ring;
static void *mpeg_mem, *ddrtop, *sps_pps;
static SceMpegAu *au;
static int sps_size, pps_size, nal_prefix;
static u32 *frame;

static int load_modules(void)
{
    if (modules >= 0) return modules;
    modules = 0;
    sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);
    SceUID mod = kuKernelLoadModule("flash0:/kd/mpeg_vsh.prx", 0, NULL);
    if (mod < 0) return 0;
    int status = 0;
    if (sceKernelStartModule(mod, 0, NULL, &status, NULL) < 0) return 0;
    modules = 1;
    return 1;
}

int avc_available(void) { return load_modules(); }

void avc_close(void)
{
    if (created) sceMpegDelete(&mpeg);
    if (inited) sceMpegFinish();
    created = inited = 0;
    free(mpeg_mem);
    free(au);
    free(sps_pps);
    mpeg_mem = sps_pps = NULL;
    au = NULL;
}

int avc_open(const Mp4 *m)
{
    avc_close();
    if (!load_modules()) return -1;
    if (!m->sps || !m->pps) return -2;
    if (m->width > 480 || m->height > 272) return -8;
    // Mode 4 decodes frames up to 480x272.
    int mode = 4;
    // Rebooting the Media Engine is only needed when the profile kind
    // changes, so it happens once for most libraries.
    static int me_type;
    int type = m->profile == 0x42 ? 4 : 3;
    if (type != me_type && boroMeBoot(type) >= 0) me_type = type;
    if (sceMpegInit() != 0) return -3;
    inited = 1;
    int size = sceMpegQueryMemSize(mode);
    if (size <= 0) { avc_close(); return -4; }
    mpeg_mem = memalign(64, size);
    // The decoder wants 2MB of working memory on a 4MB boundary; it stays
    // allocated between videos.
    if (!ddrtop) ddrtop = memalign(0x400000, 0x200000);
    if (!frame) frame = memalign(64, AVC_STRIDE * 272 * 4);
    au = memalign(64, 64);
    sps_pps = memalign(64, m->sps_size + m->pps_size);
    if (!mpeg_mem || !ddrtop || !au || !sps_pps || !frame) { avc_close(); return -5; }
    memset(&ring, 0, sizeof(ring));
    if (sceMpegCreate(&mpeg, mpeg_mem, size, &ring, AVC_STRIDE, mode, (SceInt32)ddrtop) != 0) { avc_close(); return -6; }
    created = 1;
    memset(au, 0xFF, 64);
    if (sceMpegInitAu(&mpeg, (u8 *)ddrtop + 0x10000, au) != 0) { avc_close(); return -7; }
    memcpy(sps_pps, m->sps, m->sps_size);
    memcpy((u8 *)sps_pps + m->sps_size, m->pps, m->pps_size);
    sps_size = m->sps_size;
    pps_size = m->pps_size;
    nal_prefix = m->nal_length;
    return 0;
}

int avc_decode(const void *data, int size, int first, int convert, AvcOut out, void *ctx)
{
    if (!created) return -1;
    AvcNal nal = { sps_pps, sps_size, (u8 *)sps_pps + sps_size, pps_size, nal_prefix, data, size, first ? 3 : 0 };
    sceKernelDcacheWritebackInvalidateAll();
    if (sceMpegGetAvcNalAu(&mpeg, &nal, au) != 0) return -2;
    SceInt32 pics = 0;
    if (sceMpegAvcDecode(&mpeg, au, AVC_STRIDE, NULL, &pics) != 0) return -3;
    AvcDetail2 *d = NULL;
    if (sceMpegAvcDecodeDetail2(&mpeg, &d) != 0) return -4;
    if (pics <= 0) return 0;
    if (!d || !d->info || !d->yuv) return -5;
    for (int i = 0; i < pics; i++) {
        const AvcInfo *info = d->info + i;
        const AvcYuv *yuv = d->yuv + i;
        if (info->width <= 0 || info->height <= 0 || info->width > 480 || info->height > 272) continue;
        if (!convert) { out(NULL, info->width, info->height, ctx); continue; }
        AvcCsc csc = { (info->height + 15) >> 4, (info->width + 15) >> 4, 0, 0, { 0 } };
        memcpy(csc.buffer, yuv->buffer, sizeof(csc.buffer));
        if (sceMpegBaseCscAvc(frame, 0, AVC_STRIDE, &csc) != 0) return -6;
        sceKernelDcacheInvalidateRange(frame, AVC_STRIDE * 272 * 4);
        out(frame, info->width, info->height, ctx);
    }
    return pics;
}
