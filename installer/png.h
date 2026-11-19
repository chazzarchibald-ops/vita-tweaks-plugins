// Reads a PNG into plain pixels, and fits a picture into a frame: what turns a
// project's screenshot from GitHub into the photo the app shows. The PNGs
// read are the ordinary kind: 8 bits a channel, not interlaced; grey, colour
// or a palette, with or without alpha (which is dropped).
#ifndef PNG_H
#define PNG_H

#include <stdint.h>

// The picture as w x h pixels of red, green and blue, a byte each: malloc'd.
// NULL if it isn't a PNG this reads, or is over max_pixels.
uint8_t *png_decode(const uint8_t *png, uint32_t len, uint32_t max_pixels, int *w, int *h);

// A picture of w x h pixels made to fit a frame of fw x fh, its shape kept,
// centred over fill where it doesn't cover (0xBBGGRR): fw x fh pixels of red,
// green and blue, malloc'd. Making it smaller averages the pixels each new
// one covers.
uint8_t *photo_fit(const uint8_t *rgb, int w, int h, int fw, int fh, uint32_t fill);

#endif
