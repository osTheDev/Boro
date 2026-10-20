// System volume readout. Homebrew can't read the volume from user mode, so a
// small kernel module (boro_kernel.prx, next to the EBOOT) reads it for us.
// Without the module everything here quietly does nothing.
#ifndef BORO_VOLUME_H
#define BORO_VOLUME_H

#define VOLUME_MAX 30

void volume_init(const char *app_dir);
void volume_frame(float dt); // call once per frame
int volume_level(void);      // 0..VOLUME_MAX, or -1 when unknown
float volume_shown(void);    // overlay opacity, 0 when hidden

#endif
