// Platform services: exit callback, buttons, clock and battery.
#ifndef BORO_SYS_H
#define BORO_SYS_H

#include <psptypes.h>

typedef struct {
    u32 held;    // buttons down this frame
    u32 pressed; // buttons that went down this frame
    u32 nav;     // pressed, plus auto-repeat while the d-pad or L/R is held
    int stick_x; // analog stick, -128 (left) to 127 (right)
    int stick_y; // -128 (up) to 127 (down)
    u32 stick_dirs; // d-pad bits that come from the stick being pushed
} Input;

void sys_init(void);
int sys_running(void);
void sys_quit(void);
void sys_read_input(Input *in);
void sys_clock(char *out, int size); // "9:41 PM"
int sys_battery(int *charging);      // percent, or -1 without a battery

#endif
