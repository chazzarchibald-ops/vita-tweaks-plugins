#include "gfx.h"

#include <math.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/pvf.h>
#include <psp2/sysmodule.h>
#include <stdlib.h>
#include <string.h>

#define FB_BYTES 0x200000                   // 960 x 544 x 4, rounded up to the 256 KB CDRAM wants
#define LAYER_BYTES (SCREEN_W * SCREEN_H * 4) // a multiple of 4 KB as it is
#define LINE_H_MAX 96
#define CELL_W 160 // room for the widest letter at the biggest size, and to its left

static uint32_t *fbs[2];
static int back;
// The overlay and the scene made from it come in pairs: the liquid draws from
// one pair (layers) while the next look is drawn into the other (overlay,
// scene), and gfx_show swaps them. So the screen can be redrawn on another
// thread without the liquid ever seeing it half done.
static uint32_t *base, *overlay, *scene, *saved;
static uint32_t *overlays[2], *scenes[2];
static int drawing; // which pair is being drawn into
static LiquidLayers layers;
static ScePvfLibId pvf_lib;
static ScePvfFontId fonts[2]; // by GfxFont
static ScePvfFontId pvf_font; // the one in use
static uint8_t cell_buf[CELL_W * LINE_H_MAX]; // one letter as the library draws it, 8 bits a pixel

static void *alloc_block(const char *name, SceKernelMemBlockType type, unsigned size) {
	void *ptr = NULL;
	SceUID block = sceKernelAllocMemBlock(name, type, size, NULL);
	if (block < 0 || sceKernelGetMemBlockBase(block, &ptr) < 0)
		return NULL;
	return ptr;
}

static ScePvfPointer pvf_alloc(ScePvfPointer user, ScePvfU32 size) {
	(void)user;
	return malloc(size);
}

static ScePvfPointer pvf_realloc(ScePvfPointer user, ScePvfPointer old, ScePvfU32 size) {
	(void)user;
	return realloc(old, size);
}

static void pvf_free(ScePvfPointer user, ScePvfPointer ptr) {
	(void)user;
	free(ptr);
}

// The backdrop: a dark gradient, dithered so it doesn't band.
static void draw_base(void) {
	static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
	static const int top[3] = {14, 16, 24}, bottom[3] = {28, 34, 62};
	for (int y = 0; y < SCREEN_H; y++) {
		int v[3];
		for (int ch = 0; ch < 3; ch++)
			v[ch] = top[ch] * 256 + ((bottom[ch] - top[ch]) * 256 * y) / (SCREEN_H - 1);
		uint32_t *row = base + y * SCREEN_W;
		for (int x = 0; x < SCREEN_W; x++) {
			int d = bayer[y & 3][x & 3] * 16;
			row[x] = 0xFF000000u | RGB((v[0] + d) >> 8, (v[1] + d) >> 8, (v[2] + d) >> 8);
		}
	}
}

// A font file of the VPK, read whole and opened from memory (the library
// would otherwise keep going back to the file for each letter).
static ScePvfFontId open_font(const char *path) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return NULL;
	int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	void *data = size > 0 ? malloc((size_t)size) : NULL;
	bool whole = data && sceIoRead(fd, data, (SceSize)size) == size;
	sceIoClose(fd);
	ScePvfError err = 0;
	ScePvfFontId font = whole ? scePvfOpenUserMemory(pvf_lib, data, (ScePvfU32)size, &err) : NULL;
	if (!font || err) {
		free(data);
		return NULL;
	}
	return font; // the font reads from data for as long as the app runs
}

// The system's own Latin font, for when the VPK's can't be opened.
static ScePvfFontId open_system_font(void) {
	ScePvfError err = 0;
	ScePvfFontId font = scePvfOpenDefaultLatinFontOnSharedMemory(pvf_lib, &err);
	if (font && !err)
		return font;
	// The shared font isn't there when the home screen didn't open it: its file instead.
	err = 0;
	font = scePvfOpenUserFile(pvf_lib, (ScePvfPointer) "sa0:data/font/pvf/ltn0.pvf", 0, &err);
	return err ? NULL : font;
}

bool gfx_init(void) {
	fbs[0] = alloc_block("installer_fb0", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_BYTES);
	fbs[1] = alloc_block("installer_fb1", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_BYTES);
	base = alloc_block("installer_base", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, LAYER_BYTES);
	overlays[0] = alloc_block("installer_overlay0", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, LAYER_BYTES);
	overlays[1] = alloc_block("installer_overlay1", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, LAYER_BYTES);
	scenes[0] = alloc_block("installer_scene0", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, LAYER_BYTES);
	scenes[1] = alloc_block("installer_scene1", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, LAYER_BYTES);
	saved = alloc_block("installer_saved", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, LAYER_BYTES);
	if (!fbs[0] || !fbs[1] || !base || !overlays[0] || !overlays[1] || !scenes[0] || !scenes[1] || !saved)
		return false;
	draw_base();
	for (int i = 0; i < 2; i++) {
		memset(overlays[i], 0, LAYER_BYTES);
		memcpy(scenes[i], base, LAYER_BYTES);
	}
	// the first pair is on screen, the second is drawn into
	layers.base = base;
	layers.overlay = overlays[0];
	layers.scene = scenes[0];
	drawing = 1;
	overlay = overlays[1];
	scene = scenes[1];

	sceSysmoduleLoadModule(SCE_SYSMODULE_PGF); // the font library, libpvf
	ScePvfInitRec rec;
	memset(&rec, 0, sizeof(rec));
	rec.maxNumFonts = 4;
	rec.allocFunc = pvf_alloc;
	rec.reallocFunc = pvf_realloc;
	rec.freeFunc = pvf_free;
	ScePvfError err = 0;
	pvf_lib = scePvfNewLib(&rec, &err);
	if (!pvf_lib)
		return true; // no text, but the buttons still work
	scePvfSetResolution(pvf_lib, 72.0f, 72.0f); // a point is a pixel
	fonts[FONT_TEXT] = open_font("app0:font/Poppins-Regular.ttf");
	if (!fonts[FONT_TEXT])
		fonts[FONT_TEXT] = open_system_font();
	fonts[FONT_TITLE] = open_font("app0:font/Righteous-Regular.ttf");
	if (!fonts[FONT_TITLE])
		fonts[FONT_TITLE] = fonts[FONT_TEXT];
	pvf_font = fonts[FONT_TEXT];
	return true;
}

bool gfx_has_font(void) {
	return pvf_font != NULL;
}

void gfx_font(GfxFont font) {
	pvf_font = fonts[font];
}

const LiquidLayers *gfx_layers(void) {
	return &layers;
}

void gfx_begin(void) {
	memset(overlay, 0, LAYER_BYTES);
}

void gfx_save(void) {
	memcpy(saved, overlay, LAYER_BYTES);
}

void gfx_restore(void) {
	memcpy(overlay, saved, LAYER_BYTES);
}

void gfx_show(void) {
	layers.overlay = overlay;
	layers.scene = scene;
	drawing ^= 1;
	overlay = overlays[drawing];
	scene = scenes[drawing];
}

void gfx_end(void) {
	for (int p = 0; p < SCREEN_W * SCREEN_H; p++) {
		uint32_t o = overlay[p], b = base[p];
		int oa = (int)(o >> 24);
		if (!oa) {
			scene[p] = b;
			continue;
		}
		int keep = 256 - oa - (oa >> 7);
		int r = (int)(o & 0xFF) + ((int)((b & 0xFF) * keep) >> 8);
		int g = (int)((o >> 8) & 0xFF) + ((int)(((b >> 8) & 0xFF) * keep) >> 8);
		int bl = (int)((o >> 16) & 0xFF) + ((int)(((b >> 16) & 0xFF) * keep) >> 8);
		scene[p] = 0xFF000000u | RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, bl > 255 ? 255 : bl);
	}
}

// The rows drawing is kept to, as pixel numbers: all of them, unless gfx_clip_rows says otherwise.
static int clip_from, clip_to = SCREEN_W * SCREEN_H;

void gfx_clip_rows(int top, int bottom) {
	if (top < 0)
		top = 0;
	if (bottom > SCREEN_H)
		bottom = SCREEN_H;
	clip_from = top * SCREEN_W;
	clip_to = bottom * SCREEN_W;
}

// rgb at alpha a (0 to 255) over the overlay's pixel p, which is premultiplied.
static inline void over_px(int p, uint32_t rgb, int a) {
	if (a <= 0 || p < clip_from || p >= clip_to)
		return;
	if (a > 255)
		a = 255;
	uint32_t d = overlay[p];
	int sa = a + (a >> 7), keep = 256 - sa;
	int r = (int)(((rgb & 0xFF) * sa) >> 8) + (int)(((d & 0xFF) * keep) >> 8);
	int g = (int)((((rgb >> 8) & 0xFF) * sa) >> 8) + (int)((((d >> 8) & 0xFF) * keep) >> 8);
	int b = (int)((((rgb >> 16) & 0xFF) * sa) >> 8) + (int)((((d >> 16) & 0xFF) * keep) >> 8);
	int da = a + (int)(((d >> 24) * keep) >> 8);
	overlay[p] = ((uint32_t)(da > 255 ? 255 : da) << 24) | RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

// How far inside a rounded rectangle a pixel's centre is (negative: outside).
static inline float inside_rounded(float px, float py, float cx, float cy, float hw, float hh, float radius) {
	float qx = fabsf(px - cx) - (hw - radius), qy = fabsf(py - cy) - (hh - radius);
	float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
	float outside = ox > 0.0f && oy > 0.0f ? sqrtf(ox * ox + oy * oy) : ox + oy;
	float inner = qx > qy ? qx : qy;
	return radius - outside - (inner < 0.0f ? inner : 0.0f);
}

void gfx_panel(int x, int y, int w, int h, int radius, uint32_t rgb, int alpha) {
	float cx = (float)x + (float)w * 0.5f, cy = (float)y + (float)h * 0.5f;
	for (int j = y < 0 ? 0 : y; j < y + h && j < SCREEN_H; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < SCREEN_W; i++) {
			float d = inside_rounded((float)i + 0.5f, (float)j + 0.5f, cx, cy, (float)w * 0.5f, (float)h * 0.5f,
			                         (float)radius);
			if (d <= -0.5f)
				continue;
			float cover = d >= 0.5f ? 1.0f : d + 0.5f;
			int p = j * SCREEN_W + i;
			over_px(p, rgb, (int)((float)alpha * cover));
			// the edge: a pixel-wide lighter line just inside, stronger at the top
			float edge = 1.0f - fabsf(d - 1.0f);
			if (edge > 0.0f) {
				int strength = 26 + (22 * (y + h - j)) / h;
				over_px(p, RGB(255, 255, 255), (int)((float)strength * edge));
			}
		}
}

void gfx_image(int x, int y, int w, int h, int radius, const uint8_t *rgb) {
	float cx = (float)x + (float)w * 0.5f, cy = (float)y + (float)h * 0.5f;
	for (int j = 0; j < h; j++) {
		int sy = y + j;
		if (sy < 0 || sy >= SCREEN_H || sy * SCREEN_W < clip_from || sy * SCREEN_W >= clip_to)
			continue;
		const uint8_t *row = rgb + (size_t)j * (size_t)w * 3;
		// only the rows the corners are in need the distance worked out
		bool corner = j < radius || j >= h - radius;
		for (int i = 0; i < w; i++) {
			int sx = x + i;
			if (sx < 0 || sx >= SCREEN_W)
				continue;
			int alpha = 255;
			if (corner && (i < radius || i >= w - radius)) {
				float d = inside_rounded((float)sx + 0.5f, (float)sy + 0.5f, cx, cy, (float)w * 0.5f, (float)h * 0.5f,
				                         (float)radius);
				if (d <= -0.5f)
					continue;
				if (d < 0.5f)
					alpha = (int)(255.0f * (d + 0.5f));
			}
			over_px(sy * SCREEN_W + sx, RGB(row[i * 3], row[i * 3 + 1], row[i * 3 + 2]), alpha);
		}
	}
}

void gfx_dot(int cx, int cy, int radius, uint32_t rgb, int alpha) {
	for (int j = cy - radius - 1; j <= cy + radius + 1; j++)
		for (int i = cx - radius - 1; i <= cx + radius + 1; i++) {
			if (i < 0 || j < 0 || i >= SCREEN_W || j >= SCREEN_H)
				continue;
			float dx = (float)(i - cx), dy = (float)(j - cy);
			float d = (float)radius + 0.5f - sqrtf(dx * dx + dy * dy);
			if (d > 0.0f)
				over_px(j * SCREEN_W + i, rgb, (int)((float)alpha * (d > 1.0f ? 1.0f : d)));
		}
}

// ---- letters ----
// The font library draws a letter from its outline every time it is asked,
// which is far too slow to do for every letter on every redraw. So each letter
// is drawn once at each size it is used at and its picture kept, with how far
// it moves the pen.
typedef struct {
	uint16_t key_size; // quarter pixels; 0 for a free slot
	uint8_t key_font, key_char;
	int8_t left, top; // where its picture goes from the pen and the line's top
	uint8_t w, h;
	int16_t advance64;
	uint8_t *bits; // w x h, 8 bits a pixel; NULL for a letter with nothing to draw
} Glyph;

#define GLYPHS 4096 // slots: far more than the letters and sizes the app uses
static Glyph glyphs[GLYPHS];
static int glyph_count;

static const Glyph *glyph_get(float size, unsigned char ch) {
	static Glyph scratch; // for when the table is full: drawn afresh each time, as before
	int font = pvf_font == fonts[FONT_TITLE] && fonts[FONT_TITLE] != fonts[FONT_TEXT] ? 1 : 0;
	unsigned key_size = (unsigned)(size * 4.0f + 0.5f);
	unsigned slot = (ch * 31u + key_size * 131u + (unsigned)font * 7919u) % GLYPHS;
	Glyph *g = NULL;
	for (int tries = 0; tries < GLYPHS; tries++, slot = (slot + 1) % GLYPHS) {
		g = &glyphs[slot];
		if (!g->key_size)
			break;
		if (g->key_size == key_size && g->key_font == font && g->key_char == ch)
			return g;
	}
	if (glyph_count >= GLYPHS * 3 / 4) {
		free(scratch.bits);
		g = &scratch;
	} else {
		glyph_count++;
	}
	memset(g, 0, sizeof(*g));
	g->key_size = (uint16_t)key_size;
	g->key_font = (uint8_t)font;
	g->key_char = ch;

	scePvfSetCharSize(pvf_font, size, size);
	ScePvfCharInfo info;
	memset(&info, 0, sizeof(info));
	if (scePvfGetCharInfo(pvf_font, ch, &info) < 0)
		return g; // a letter the font doesn't have: nothing, and no width
	g->advance64 = (int16_t)info.glyphMetrics.horizontalAdvance64;
	if (ch == ' ')
		return g;
	// drawn into a cell with room to the pen's left, for letters that reach back
	int line_h = (int)(size * 1.5f) + 2, pad = (int)size / 2 + 2;
	if (line_h > LINE_H_MAX)
		line_h = LINE_H_MAX;
	int cell_w = pad + (int)size * 2 + 8;
	if (cell_w > CELL_W)
		cell_w = CELL_W;
	memset(cell_buf, 0, (size_t)(CELL_W * line_h));
	ScePvfUserImageBufferRec image;
	memset(&image, 0, sizeof(image));
	image.pixelFormat = SCE_PVF_USERIMAGE_DIRECT8;
	image.xPos64 = pad * 64;
	image.yPos64 = (int)(size * 1.12f) * 64;
	image.rect.width = cell_w;
	image.rect.height = line_h;
	image.bytesPerLine = CELL_W;
	image.buffer = cell_buf;
	scePvfGetCharGlyphImage(pvf_font, ch, &image);
	// the part of the cell it drew on
	int x0 = cell_w, x1 = -1, y0 = line_h, y1 = -1;
	for (int j = 0; j < line_h; j++)
		for (int i = 0; i < cell_w; i++)
			if (cell_buf[j * CELL_W + i]) {
				if (i < x0)
					x0 = i;
				if (i > x1)
					x1 = i;
				if (j < y0)
					y0 = j;
				if (j > y1)
					y1 = j;
			}
	if (x1 < 0)
		return g;
	int w = x1 - x0 + 1, h = y1 - y0 + 1;
	g->bits = malloc((size_t)(w * h));
	if (!g->bits)
		return g;
	for (int j = 0; j < h; j++)
		memcpy(g->bits + j * w, cell_buf + (y0 + j) * CELL_W + x0, (size_t)w);
	g->w = (uint8_t)w;
	g->h = (uint8_t)h;
	g->left = (int8_t)(x0 - pad);
	g->top = (int8_t)y0;
	return g;
}

int gfx_text_width(float size, const char *s) {
	if (!pvf_font)
		return 0;
	int pen64 = 0;
	for (const unsigned char *c = (const unsigned char *)s; *c; c++)
		pen64 += glyph_get(size, *c)->advance64;
	return (pen64 + 63) / 64;
}

int gfx_text(int x, int y, float size, uint32_t rgb, int alpha, const char *s) {
	if (!pvf_font || !s[0])
		return 0;
	// a line wholly outside the rows being drawn (scrolled out of its space): only its width is wanted
	if ((y + LINE_H_MAX) * SCREEN_W <= clip_from || y * SCREEN_W >= clip_to)
		return gfx_text_width(size, s);
	int pen64 = 0;
	for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
		const Glyph *g = glyph_get(size, *c);
		if (g->bits) {
			int gx = x + ((pen64 + 32) >> 6) + g->left, gy = y + g->top;
			for (int j = 0; j < g->h; j++) {
				int sy = gy + j;
				if (sy < 0 || sy >= SCREEN_H)
					continue;
				const uint8_t *row = g->bits + j * g->w;
				for (int i = 0; i < g->w; i++) {
					int sx = gx + i;
					if (row[i] && sx >= 0 && sx < SCREEN_W)
						over_px(sy * SCREEN_W + sx, rgb, (row[i] * alpha) / 255);
				}
			}
		}
		pen64 += g->advance64;
	}
	return (pen64 + 63) / 64;
}

int gfx_text_shadow(int x, int y, float size, uint32_t rgb, const char *s) {
	gfx_text(x + 1, y + 2, size, RGB(0, 0, 0), 150, s);
	gfx_text(x - 1, y + 1, size, RGB(0, 0, 0), 90, s);
	return gfx_text(x, y, size, rgb, 255, s);
}

int gfx_text_wrap(int x, int y, int width, float size, int line_h, uint32_t rgb, const char *s) {
	char line[160];
	int len = 0;
	line[0] = '\0';
	while (*s) {
		while (*s == ' ')
			s++;
		const char *end = s;
		while (*end && *end != ' ')
			end++;
		int word = (int)(end - s);
		if (!word)
			break;
		int add = word + (len ? 1 : 0);
		if (len + add >= (int)sizeof(line))
			break;
		// Try the word on this line; if it's too wide, it starts the next.
		if (len)
			line[len] = ' ';
		memcpy(line + len + (len ? 1 : 0), s, (size_t)word);
		line[len + add] = '\0';
		if (len && gfx_text_width(size, line) > width) {
			line[len] = '\0';
			gfx_text(x, y, size, rgb, 255, line);
			y += line_h;
			memcpy(line, s, (size_t)word);
			len = word;
			line[len] = '\0';
		} else {
			len += add;
		}
		s = end;
	}
	if (len) {
		gfx_text(x, y, size, rgb, 255, line);
		y += line_h;
	}
	return y;
}

uint32_t *gfx_back(void) {
	return fbs[back];
}

int gfx_back_index(void) {
	return back;
}

void gfx_present(void) {
	SceDisplayFrameBuf param;
	memset(&param, 0, sizeof(param));
	param.size = sizeof(param);
	param.base = fbs[back];
	param.pitch = SCREEN_W;
	param.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	param.width = SCREEN_W;
	param.height = SCREEN_H;
	sceDisplaySetFrameBuf(&param, SCE_DISPLAY_SETBUF_NEXTFRAME);
	sceDisplayWaitVblankStart();
	back ^= 1;
}
