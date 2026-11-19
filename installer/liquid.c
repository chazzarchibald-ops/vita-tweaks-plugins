// A ball of liquid, drawn on the CPU.
//
// It's a handful of metaballs: a main ball that follows a looping path, four
// smaller ones that follow the same path a little later each (so the ball
// stretches into a tail when it moves fast and gathers again when it slows),
// and two droplets that orbit it. Each adds a smooth bump to a field; the
// liquid is where the field is above a threshold, so balls that come close
// flow into one another.
//
// The field is kept at half the screen's resolution and smoothed back up to
// full resolution for every pixel, which gives clean edges at a quarter of
// the cost. Lighting is worked out at the same half resolution: the field,
// levelled off towards the top, is the surface's height (a drop with a round
// edge and a flattish top, however many balls are piled up inside it), so
// its slope gives each point a normal, and from that a diffuse light from the
// upper left, a highlight, and a brighter rim. Only the 16 x 16 tiles the
// liquid touches are drawn, across three threads.
//
// The Vita's GPU would do this in a shader, but the SDK here has no shader
// compiler for it, and compiling shaders on the console needs a module most
// people would have to install first. The CPU manages it.
//
// preview/liquid.js is this file line for line, for the preview page.

#include "liquid.h"

#include <math.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>

#define TILE 16
#define TILES_X (SCREEN_W / TILE)
#define TILES_Y (SCREEN_H / TILE)
#define TILES (TILES_X * TILES_Y)
#define TILE_CELLS (TILE / 2)
#define GRID_W (SCREEN_W / 2)
#define GRID_H (SCREEN_H / 2)
#define GM 2 // a margin of cells around the grid, so neighbours always exist
#define STRIDE (GRID_W + 2 * GM)
#define GRID_SIZE (STRIDE * (GRID_H + 2 * GM))
#define CELL(i, j) (((j) + GM) * STRIDE + (i) + GM)

#define F_T 1024    // the field's value at the liquid's surface (a ball's centre is 4096)
#define F_GLOW 64   // the glow around the liquid starts here
#define F_LIT 256   // lighting is worked out from here up
// The surface's height is f / (f + HEIGHT_A): it rises steeply at the edge and
// levels off inside, so the balls hidden in the middle of the liquid don't
// show as bumps. SLOPE_K is how tall it's lit as: more is rounder.
#define HEIGHT_A 1100.0f
#define SLOPE_K 120.0f
// the light, 128 being full brightness
#define LIGHT_AMBIENT 56.0f
#define LIGHT_DIFFUSE 80.0f
#define LIGHT_RIM 60.0f
#define RAMP_N 2048 // the colours, by field value / 32
#define MAX_BALLS 8

typedef struct {
	float x, y;   // centre, in pixels
	float r;      // the bump's radius: the ball you see is 0.707 of it
	float ux, uy; // the direction it's stretched along
	float sa, sb; // how much, along and across
} Ball;

static float *field;    // the summed bumps: 4096 at a ball's centre
static uint32_t *fc;    // field (bits 0-15) and distance to the surface (bits 20-27)
static uint32_t *ls;    // light (bits 0-7) and highlight (bits 16-23)
static uint8_t tiles_now[TILES], tiles_field[TILES], tiles_prev[2][TILES];
static uint32_t ramp[RAMP_N];
static uint32_t glow_r, glow_g, glow_b;

// what the threads are working on
static uint32_t *job_fb;
static const uint8_t *job_prev;
static bool job_full;
static const LiquidLayers *job_layers;

static inline int clampi(int v, int lo, int hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

// ---- the balls ----

// Times are doubles: the app can sit open for hours, and a float's seconds
// would get coarse enough to make the liquid stutter.
static void path(double s, float *x, float *y) {
	*x = (float)(480.0 + 300.0 * sin(0.53 * s) + 90.0 * sin(0.97 * s + 1.3));
	*y = (float)(272.0 + 140.0 * sin(0.71 * s + 0.4) + 50.0 * sin(1.31 * s + 2.1));
}

static void set_ball(Ball *b, float x, float y, float radius, float ux, float uy, float sa, float sb) {
	b->x = x;
	b->y = y;
	b->r = radius * 1.41421f;
	b->ux = ux;
	b->uy = uy;
	b->sa = sa;
	b->sb = sb;
}

static int place_balls(double t, Ball *b) {
	static const float trail_r[4] = {56.0f, 42.0f, 30.0f, 20.0f};
	double tau = t + 0.55 * sin(0.70 * t); // it surges, then lingers
	float x, y, xa, ya, xb, yb;
	path(tau, &x, &y);
	path(tau - 0.02, &xa, &ya);
	path(tau + 0.02, &xb, &yb);
	float vx = (xb - xa) / 0.04f, vy = (yb - ya) / 0.04f;
	float speed = sqrtf(vx * vx + vy * vy);
	float k = speed / 420.0f;
	if (k > 1.0f)
		k = 1.0f;
	float ux = 1.0f, uy = 0.0f;
	if (speed > 1.0f) {
		ux = vx / speed;
		uy = vy / speed;
	}
	int n = 0;
	set_ball(&b[n++], x, y, 74.0f + 5.0f * (float)sin(2.1 * t), ux, uy, 1.0f + 0.30f * k, 1.0f - 0.16f * k);
	for (int i = 1; i <= 4; i++) {
		float tx, ty;
		path(tau - 0.16 * i, &tx, &ty);
		tx += 8.0f * (float)sin(1.7 * t + i * 1.9);
		ty += 8.0f * (float)cos(1.3 * t + i * 2.3);
		set_ball(&b[n++], tx, ty, trail_r[i - 1], 1.0f, 0.0f, 1.0f, 1.0f);
	}
	for (int i = 0; i < 2; i++) {
		double a = 1.9 * t + i * 3.3;
		float orbit = 86.0f + 34.0f * (float)sin(1.3 * t + i * 2.0);
		set_ball(&b[n++], x + orbit * (float)cos(a), y + orbit * (float)sin(a) * 0.8f, i ? 14.0f : 18.0f, 1.0f,
		         0.0f, 1.0f, 1.0f);
	}
	return n;
}

static void mark_tiles(const Ball *b) {
	float rmax = b->r * (b->sa > b->sb ? b->sa : b->sb) + 4.0f;
	int x0 = clampi((int)floorf((b->x - rmax) / TILE), 0, TILES_X - 1);
	int x1 = clampi((int)floorf((b->x + rmax) / TILE), 0, TILES_X - 1);
	int y0 = clampi((int)floorf((b->y - rmax) / TILE), 0, TILES_Y - 1);
	int y1 = clampi((int)floorf((b->y + rmax) / TILE), 0, TILES_Y - 1);
	if (b->x + rmax < 0 || b->y + rmax < 0 || b->x - rmax >= SCREEN_W || b->y - rmax >= SCREEN_H)
		return;
	for (int ty = y0; ty <= y1; ty++)
		for (int tx = x0; tx <= x1; tx++)
			tiles_now[ty * TILES_X + tx] = 1;
}

// Adds a ball's bump to the field: (1 - d2/r2) squared, d being the distance
// from its centre, measured along and across its stretch.
static void splat(const Ball *b) {
	float rmax = b->r * (b->sa > b->sb ? b->sa : b->sb);
	int x0 = (int)floorf((b->x - rmax) * 0.5f), x1 = (int)ceilf((b->x + rmax) * 0.5f);
	int y0 = (int)floorf((b->y - rmax) * 0.5f), y1 = (int)ceilf((b->y + rmax) * 0.5f);
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > GRID_W - 1)
		x1 = GRID_W - 1;
	if (y1 > GRID_H - 1)
		y1 = GRID_H - 1;
	float ra = b->r * b->sa, rb = b->r * b->sb;
	float ia = 1.0f / (ra * ra), ib = 1.0f / (rb * rb);
	for (int j = y0; j <= y1; j++) {
		float dy = (float)(2 * j + 1) - b->y;
		float *row = field + CELL(x0, j);
		for (int i = x0; i <= x1; i++, row++) {
			float dx = (float)(2 * i + 1) - b->x;
			float u = dx * b->ux + dy * b->uy, w = dy * b->ux - dx * b->uy;
			float q = 1.0f - (u * u * ia + w * w * ib);
			if (q > 0.0f)
				*row += q * q * 4096.0f;
		}
	}
}

// ---- lighting, at the grid's resolution ----

static void light_tile(int tx, int ty) {
	for (int j = ty * TILE_CELLS; j < (ty + 1) * TILE_CELLS; j++) {
		int idx = CELL(tx * TILE_CELLS, j);
		for (int i = 0; i < TILE_CELLS; i++, idx++) {
			float f = field[idx];
			if (f <= 0.0f) {
				fc[idx] = 0;
				ls[idx] = 0;
				continue;
			}
			// the field's slope, across 4 pixels
			float gx = field[idx + 1] - field[idx - 1];
			float gy = field[idx + STRIDE] - field[idx - STRIDE];
			// How far inside the surface this point is, in pixels times 24, around
			// 128: the field's height over the surface's, divided by its slope.
			float mag = sqrtf(gx * gx + gy * gy);
			float far = (f - (float)F_T) * 96.0f;
			int c = far >= 127.0f * mag ? 255 : far <= -127.0f * mag ? 1 : clampi(128 + (int)(far / mag), 1, 255);
			int fi = f > 65535.0f ? 65535 : (int)f;
			fc[idx] = (uint32_t)fi | ((uint32_t)c << 20);
			if (f < (float)F_LIT) {
				ls[idx] = 0;
				continue;
			}
			// The height's slope (gx and gy are the field's, across 4 pixels)
			// leans the normal away from the middle: n = (-tx, -ty, 1) / length.
			float fa = f + HEIGHT_A;
			float k = SLOPE_K * HEIGHT_A * 0.25f / (fa * fa);
			float tx = gx * k, ty = gy * k;
			float nz = 1.0f / sqrtf(1.0f + tx * tx + ty * ty);
			// the light is up, left and in front: (-0.45, -0.55, 0.704)
			float diffuse = (tx * 0.45f + ty * 0.55f + 0.704f) * nz;
			if (diffuse < 0.0f)
				diffuse = 0.0f;
			// the highlight: halfway between the light and the eye
			// (to the 48th power: a table of 256 steps showed as bands across it)
			float half = (tx * 0.244f + ty * 0.298f + 0.923f) * nz;
			if (half < 0.0f)
				half = 0.0f;
			float h4 = half * half * half * half, h16 = h4 * h4 * h4 * h4;
			int spec = (int)(h16 * h16 * h16 * 255.0f);
			float rim = (1.0f - nz) * (1.0f - nz);
			int light = (int)(LIGHT_AMBIENT + diffuse * LIGHT_DIFFUSE + rim * LIGHT_RIM);
			if (light > 255)
				light = 255;
			ls[idx] = (uint32_t)light | ((uint32_t)spec << 16);
		}
	}
}

static void light_rows(int part, int parts) {
	for (int ty = part; ty < TILES_Y; ty += parts)
		for (int tx = 0; tx < TILES_X; tx++)
			if (tiles_now[ty * TILES_X + tx])
				light_tile(tx, ty);
}

// ---- pixels ----

// One pixel: fcv and lsv are sixteen times the field and light values there.
static inline uint32_t shade(uint32_t fcv, uint32_t lsv, uint32_t base, uint32_t over) {
	int f = (int)((fcv & 0xFFFFF) >> 4), c = (int)(fcv >> 24);
	int r = (int)(base & 0xFF), g = (int)((base >> 8) & 0xFF), b = (int)((base >> 16) & 0xFF);
	// the glow around it
	int glow = f >= F_T ? 256 : f <= F_GLOW ? 0 : ((f - F_GLOW) * 68) >> 8;
	glow = (glow * glow) >> 8;
	r += ((int)glow_r * glow) >> 9;
	g += ((int)glow_g * glow) >> 9;
	b += ((int)glow_b * glow) >> 9;
	// the liquid, with its edge smoothed over about a pixel
	int cover = 128 + (c - 128) * 8;
	if (cover > 0) {
		if (cover > 256)
			cover = 256;
		int light = (int)((lsv >> 4) & 0xFF), spec = (int)((lsv >> 20) & 0xFF);
		uint32_t col = ramp[f >> 5];
		int lr = (((int)(col & 0xFF) * light) >> 7) + spec;
		int lg = (((int)((col >> 8) & 0xFF) * light) >> 7) + spec;
		int lb = (((int)((col >> 16) & 0xFF) * light) >> 7) + spec;
		r += ((lr - r) * cover) >> 8;
		g += ((lg - g) * cover) >> 8;
		b += ((lb - b) * cover) >> 8;
	}
	// the text and panels over it
	int oa = (int)(over >> 24);
	int keep = 256 - oa - (oa >> 7);
	r = (int)(over & 0xFF) + ((r * keep) >> 8);
	g = (int)((over >> 8) & 0xFF) + ((g * keep) >> 8);
	b = (int)((over >> 16) & 0xFF) + ((b * keep) >> 8);
	if (r > 255)
		r = 255;
	if (g > 255)
		g = 255;
	if (b > 255)
		b = 255;
	return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void copy_tile(int tx, int ty) {
	const uint32_t *src = job_layers->scene + ty * TILE * SCREEN_W + tx * TILE;
	uint32_t *dst = job_fb + ty * TILE * SCREEN_W + tx * TILE;
	for (int y = 0; y < TILE; y++, src += SCREEN_W, dst += SCREEN_W)
		memcpy(dst, src, TILE * sizeof(uint32_t));
}

// A cell is 2 x 2 pixels. Each pixel takes 9/16 of its cell's value, 3/16 of
// the two cells beside it and 1/16 of the one diagonal from it.
static void shade_tile(int tx, int ty) {
	const uint32_t *base = job_layers->base, *over = job_layers->overlay, *scene = job_layers->scene;
	for (int j = ty * TILE_CELLS; j < (ty + 1) * TILE_CELLS; j++) {
		const uint32_t *fm = fc + CELL(tx * TILE_CELLS, j), *fu = fm - STRIDE, *fd = fm + STRIDE;
		const uint32_t *lm = ls + CELL(tx * TILE_CELLS, j), *lu = lm - STRIDE, *ld = lm + STRIDE;
		int p = 2 * j * SCREEN_W + tx * TILE; // the cell's top left pixel
		for (int i = 0; i < TILE_CELLS; i++, p += 2) {
			uint32_t c = fm[i], l = fm[i - 1], r = fm[i + 1], u = fu[i], d = fd[i];
			uint32_t ul = fu[i - 1], ur = fu[i + 1], dl = fd[i - 1], dr = fd[i + 1];
			if (((c | l | r | u | d | ul | ur | dl | dr) & 0xFFFF) < F_GLOW) {
				job_fb[p] = scene[p];
				job_fb[p + 1] = scene[p + 1];
				job_fb[p + SCREEN_W] = scene[p + SCREEN_W];
				job_fb[p + SCREEN_W + 1] = scene[p + SCREEN_W + 1];
				continue;
			}
			uint32_t c9 = c * 9, l3 = l * 3, r3 = r * 3, u3 = u * 3, d3 = d * 3;
			uint32_t lc = lm[i] * 9, ll = lm[i - 1] * 3, lr = lm[i + 1] * 3, lup = lu[i] * 3, ldn = ld[i] * 3;
			job_fb[p] = shade(c9 + l3 + u3 + ul, lc + ll + lup + lu[i - 1], base[p], over[p]);
			job_fb[p + 1] = shade(c9 + r3 + u3 + ur, lc + lr + lup + lu[i + 1], base[p + 1], over[p + 1]);
			job_fb[p + SCREEN_W] =
				shade(c9 + l3 + d3 + dl, lc + ll + ldn + ld[i - 1], base[p + SCREEN_W], over[p + SCREEN_W]);
			job_fb[p + SCREEN_W + 1] = shade(c9 + r3 + d3 + dr, lc + lr + ldn + ld[i + 1], base[p + SCREEN_W + 1],
			                                 over[p + SCREEN_W + 1]);
		}
	}
}

static void pixel_rows(int part, int parts) {
	for (int ty = part; ty < TILES_Y; ty += parts)
		for (int tx = 0; tx < TILES_X; tx++) {
			int t = ty * TILES_X + tx;
			if (tiles_now[t])
				shade_tile(tx, ty);
			else if (job_full || job_prev[t])
				copy_tile(tx, ty);
		}
}

// ---- the colours, which drift from one set to the next ----

static const uint8_t PALETTES[4][3][3] = {
	{{16, 44, 230}, {24, 120, 255}, {70, 200, 255}},   // blue
	{{92, 24, 228}, {160, 56, 255}, {214, 130, 255}},  // violet
	{{214, 18, 140}, {255, 54, 150}, {255, 138, 196}}, // magenta
	{{0, 124, 160}, {10, 200, 180}, {110, 246, 200}},  // teal
};
#define PALETTE_SECONDS 7.0

static void build_ramp(double t) {
	double pos = t / PALETTE_SECONDS;
	int from = (int)fmod(pos, 4.0);
	float k = (float)(pos - floor(pos));
	k = k * k * (3.0f - 2.0f * k);
	from &= 3;
	int to = (from + 1) & 3;
	int stops[3][3];
	for (int s = 0; s < 3; s++)
		for (int ch = 0; ch < 3; ch++)
			stops[s][ch] = (int)((float)PALETTES[from][s][ch] +
			                     ((float)PALETTES[to][s][ch] - (float)PALETTES[from][s][ch]) * k);
	// The edge colour at the surface, the middle one about where a ball's
	// centre is, and on towards the core where balls pile up: h never quite
	// gets to 1, so there's no line where the colour stops changing.
	for (int i = 0; i < RAMP_N; i++) {
		float over = (float)(i * 32 + 16 - F_T);
		float h = over <= 0.0f ? 0.0f : over / (over + 2600.0f);
		int a = h < 0.5f ? 0 : 1;
		float w = h < 0.5f ? h * 2.0f : h * 2.0f - 1.0f;
		int r = stops[a][0] + (int)((float)(stops[a + 1][0] - stops[a][0]) * w);
		int g = stops[a][1] + (int)((float)(stops[a + 1][1] - stops[a][1]) * w);
		int b = stops[a][2] + (int)((float)(stops[a + 1][2] - stops[a][2]) * w);
		ramp[i] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
	}
	glow_r = (uint32_t)stops[1][0];
	glow_g = (uint32_t)stops[1][1];
	glow_b = (uint32_t)stops[1][2];
}

// ---- threads: each takes every third row of tiles ----

#define WORKERS 2
static struct {
	SceUID thread, start;
} workers[WORKERS];
static SceUID done_sema = -1;
static int worker_count;
static void (*volatile job_fn)(int part, int parts);

static int worker_main(SceSize args, void *argp) {
	int index = *(const int *)argp;
	(void)args;
	for (;;) {
		sceKernelWaitSema(workers[index].start, 1, NULL);
		job_fn(index + 1, worker_count + 1);
		sceKernelSignalSema(done_sema, 1);
	}
	return 0;
}

static void in_parallel(void (*fn)(int part, int parts)) {
	job_fn = fn;
	for (int i = 0; i < worker_count; i++)
		sceKernelSignalSema(workers[i].start, 1);
	fn(0, worker_count + 1);
	for (int i = 0; i < worker_count; i++)
		sceKernelWaitSema(done_sema, 1, NULL);
}

static void start_workers(void) {
	done_sema = sceKernelCreateSema("liquid_done", 0, 0, WORKERS, NULL);
	if (done_sema < 0)
		return;
	for (int i = 0; i < WORKERS; i++) {
		workers[i].start = sceKernelCreateSema("liquid_start", 0, 0, 1, NULL);
		if (workers[i].start < 0)
			return;
		workers[i].thread = sceKernelCreateThread("liquid_worker", worker_main, 0x10000100, 0x10000, 0, 0, NULL);
		if (workers[i].thread < 0)
			return;
		// The thread reads worker_count only once it's signalled, by which
		// time every worker that's going to start has.
		int index = i;
		if (sceKernelStartThread(workers[i].thread, sizeof(index), &index) < 0)
			return;
		worker_count = i + 1;
	}
}

// ---- the frame ----

bool liquid_init(void) {
	field = calloc(GRID_SIZE, sizeof(*field));
	fc = calloc(GRID_SIZE, sizeof(*fc));
	ls = calloc(GRID_SIZE, sizeof(*ls));
	if (!field || !fc || !ls)
		return false;
	start_workers();
	return true;
}

static void clear_tile(int tx, int ty) {
	for (int j = ty * TILE_CELLS; j < (ty + 1) * TILE_CELLS; j++) {
		int idx = CELL(tx * TILE_CELLS, j);
		memset(field + idx, 0, TILE_CELLS * sizeof(*field));
		memset(fc + idx, 0, TILE_CELLS * sizeof(*fc));
		memset(ls + idx, 0, TILE_CELLS * sizeof(*ls));
	}
}

void liquid_frame(uint32_t *fb, int buf, double t, bool full, const LiquidLayers *layers) {
	// The field is only ever non-zero under the tiles marked last frame.
	for (int ty = 0; ty < TILES_Y; ty++)
		for (int tx = 0; tx < TILES_X; tx++)
			if (tiles_field[ty * TILES_X + tx])
				clear_tile(tx, ty);
	memset(tiles_now, 0, sizeof(tiles_now));
	Ball balls[MAX_BALLS];
	int count = place_balls(t, balls);
	for (int i = 0; i < count; i++)
		mark_tiles(&balls[i]);
	for (int i = 0; i < count; i++)
		splat(&balls[i]);
	build_ramp(t);

	job_fb = fb;
	job_prev = tiles_prev[buf];
	job_full = full;
	job_layers = layers;
	in_parallel(light_rows);
	in_parallel(pixel_rows);

	memcpy(tiles_prev[buf], tiles_now, sizeof(tiles_now));
	memcpy(tiles_field, tiles_now, sizeof(tiles_now));
}
