// Kernel-mode helper loaded by Boro at startup. User-mode homebrew can't read
// the system volume, so this module reads it on Boro's behalf.
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
