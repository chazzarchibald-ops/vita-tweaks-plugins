// The installer's screen: two framebuffers that take turns, the layers the
// liquid is drawn between (liquid.h), and panels and text drawn into the
// overlay. The text is in the two fonts of the LiveArea's artwork, which the
// VPK carries: Poppins, and Righteous for the name.
#ifndef GFX_H
#define GFX_H

#include "liquid.h"

#define RGB(r, g, b) (((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

typedef enum { FONT_TEXT, FONT_TITLE } GfxFont;

bool gfx_init(void);
bool gfx_has_font(void);
// The font the text calls after this one use. FONT_TEXT to begin with.
void gfx_font(GfxFont font);

// The next look of the screen is drawn between these two, out of sight: begin
// clears its overlay, end makes its scene. gfx_show then puts it on screen in
// place of the last one. Drawing may happen on another thread than the one
// that shows it and draws the liquid, as long as show waits for end.
void gfx_begin(void);
void gfx_end(void);
void gfx_show(void);
// Keeps a copy of the overlay as it is now, and puts that copy back: for the
// parts that never change.
void gfx_save(void);
void gfx_restore(void);
const LiquidLayers *gfx_layers(void);

// A rounded panel of tinted glass with a faint edge.
void gfx_panel(int x, int y, int w, int h, int radius, uint32_t rgb, int alpha);
// A filled circle.
void gfx_dot(int cx, int cy, int radius, uint32_t rgb, int alpha);
// A picture with rounded corners: w x h pixels of red, green and blue, a byte each.
void gfx_image(int x, int y, int w, int h, int radius, const uint8_t *rgb);
// Keeps what is drawn from here on to the rows from top to just above bottom,
// for something that scrolls inside a space. (0, SCREEN_H) is the whole screen again.
void gfx_clip_rows(int top, int bottom);
// Text, y being the top of its line. Returns its width.
int gfx_text(int x, int y, float size, uint32_t rgb, int alpha, const char *s);
// The same over a soft dark copy, for text with nothing behind it but the liquid.
int gfx_text_shadow(int x, int y, float size, uint32_t rgb, const char *s);
int gfx_text_width(float size, const char *s);
// Text wrapped to width, line_h apart. Returns the y after its last line.
int gfx_text_wrap(int x, int y, int width, float size, int line_h, uint32_t rgb, const char *s);

// The buffer to draw the next frame into, and which of the two it is.
uint32_t *gfx_back(void);
int gfx_back_index(void);
// Shows it at the next vertical blank, and waits for that.
void gfx_present(void);

#endif
