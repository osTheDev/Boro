// PSP menu order: the XMB lists memory stick games newest first, so putting
// games in an order here sets each ISO file's or EBOOT folder's date to
// match. Nothing is moved or renamed.
#ifndef BORO_GORDER_H
#define BORO_GORDER_H

#include "sys.h"

void gorder_open(void);
int gorder_active(void);
int gorder_update(const Input *in, float dt); // 1 closed, 2 closed after saving
void gorder_draw(float dt);

#endif
