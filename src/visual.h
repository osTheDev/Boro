// Idle visualizers: five pixel scenes drawn at half resolution in the head
// unit color, rotating every 30 seconds. Spectrum pipes, meteor rain and the
// aurora follow the music; dolphin bay and night drive just play.
#ifndef BORO_VISUAL_H
#define BORO_VISUAL_H

void visual_enter(void);
void visual_draw(float dt);
// The latest audio as n log-spaced bands, 0..1 (n up to 32). Call once a frame.
void visual_bands(float *out, int n, float dt);

#endif
