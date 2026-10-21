// The launch screen: the wordmark on the home backdrop with a progress bar,
// up while the music library loads (about a second; never more than three).
#ifndef BORO_SPLASH_H
#define BORO_SPLASH_H

int splash_update(float dt); // 1 once it's done
void splash_draw(void);

#endif
