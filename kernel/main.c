// Kernel-mode helper loaded by Boro at startup. User-mode homebrew can't read
// the system volume or boot the Media Engine for H.264, so this module does
// both on Boro's behalf.
#include <pspimpose_driver.h>
#include <pspkernel.h>
#include <pspsdk.h>

PSP_MODULE_INFO("BoroKernel", PSP_MODULE_KERNEL, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();

// The system volume, 0..30.
int boroGetVolume(void)
{
    int k1 = pspSdkSetK1(0);
    int v = sceImposeGetParam(PSP_IMPOSE_MAIN_VOLUME);
    pspSdkSetK1(k1);
    return v;
}

int sceMeBootStart660(int type); // sceMeCore_driver on 6.60 and 6.61

// Boots the Media Engine with the H.264 decoder: 3 for Main and High
// profile, 4 for Baseline.
int boroMeBoot(int type)
{
    if (sceKernelDevkitVersion() < 0x06060000) return -1;
    int k1 = pspSdkSetK1(0);
    int r = sceMeBootStart660(type);
    pspSdkSetK1(k1);
    return r;
}

int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return 0;
}
