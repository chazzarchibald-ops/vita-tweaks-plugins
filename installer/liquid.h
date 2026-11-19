// The installer's backdrop: a ball of liquid that swooshes around the screen.
#ifndef LIQUID_H
#define LIQUID_H

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 960
#define SCREEN_H 544

// The layers the liquid is drawn between, each SCREEN_W x SCREEN_H pixels of
// A8B8G8R8: base under it, overlay (premultiplied) over it, and scene, which
// is overlay over base, for where the liquid isn't.
typedef struct {
	const uint32_t *base, *overlay, *scene;
} LiquidLayers;

bool liquid_init(void);

// Draws the liquid at time t (seconds) into fb. buf says which of the two
// buffers that take turns on screen fb is (0 or 1): only the tiles the liquid
// is on now, or was on the last time this buffer was drawn, are touched,
// unless full is set (the layers changed).
void liquid_frame(uint32_t *fb, int buf, double t, bool full, const LiquidLayers *layers);

#endif
