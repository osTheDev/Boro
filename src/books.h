// Books: audiobooks in a library look (wood shelf, catalog card, an open book
// while listening, and a contents page).
#ifndef BORO_BOOKS_H
#define BORO_BOOKS_H

#include "sys.h"

void books_enter(void);
void books_enter_now(void); // continues the current book and opens Now listening
int books_update(const Input *in, float dt); // nonzero: back to home
void books_draw(float dt);

#endif
