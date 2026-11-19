// liquid.c, line for line, for the preview page and for checking frames with
// Node. Pixels are the same 0xAABBGGRR words as the Vita's framebuffer, which
// is also the byte order of a canvas's ImageData.
'use strict';

const SCREEN_W = 960, SCREEN_H = 544;
const TILE = 16, TILES_X = SCREEN_W / TILE, TILES_Y = SCREEN_H / TILE, TILES = TILES_X * TILES_Y;
const TILE_CELLS = TILE / 2;
const GRID_W = SCREEN_W / 2, GRID_H = SCREEN_H / 2, GM = 2;
const STRIDE = GRID_W + 2 * GM, GRID_SIZE = STRIDE * (GRID_H + 2 * GM);
const CELL = (i, j) => (j + GM) * STRIDE + i + GM;
const F_T = 1024, F_GLOW = 64, F_LIT = 256, RAMP_N = 2048;
const HEIGHT_A = 1100.0, SLOPE_K = 120.0, LIGHT_AMBIENT = 56.0, LIGHT_DIFFUSE = 80.0, LIGHT_RIM = 60.0;
const PALETTE_SECONDS = 7.0;
const PALETTES = [
	[[16, 44, 230], [24, 120, 255], [70, 200, 255]],   // blue
	[[92, 24, 228], [160, 56, 255], [214, 130, 255]],  // violet
	[[214, 18, 140], [255, 54, 150], [255, 138, 196]], // magenta
	[[0, 124, 160], [10, 200, 180], [110, 246, 200]],  // teal
];

const clampi = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

function createLiquid(debug) {
	const field = new Float32Array(GRID_SIZE), fc = new Uint32Array(GRID_SIZE), ls = new Uint32Array(GRID_SIZE);
	const tilesNow = new Uint8Array(TILES), tilesField = new Uint8Array(TILES);
	const tilesPrev = [new Uint8Array(TILES), new Uint8Array(TILES)];
	const ramp = new Uint32Array(RAMP_N);
	let glowR = 0, glowG = 0, glowB = 0;
	let jobFb = null, jobPrev = null, jobFull = false, jobLayers = null;
	const stats = { cells: 0, shaded: 0, copied: 0, tiles: 0 };

	const check = (index, size, what) => {
		if (index < 0 || index >= size || index !== (index | 0))
			throw new Error(what + ' index ' + index + ' outside 0..' + size);
	};


	// ---- the balls ----

	const path = (s) => [
		480.0 + 300.0 * Math.sin(0.53 * s) + 90.0 * Math.sin(0.97 * s + 1.3),
		272.0 + 140.0 * Math.sin(0.71 * s + 0.4) + 50.0 * Math.sin(1.31 * s + 2.1),
	];

	const ball = (x, y, radius, ux, uy, sa, sb) => ({ x, y, r: radius * 1.41421, ux, uy, sa, sb });

	function placeBalls(t) {
		const trailR = [56.0, 42.0, 30.0, 20.0];
		const tau = t + 0.55 * Math.sin(0.70 * t);
		const [x, y] = path(tau), [xa, ya] = path(tau - 0.02), [xb, yb] = path(tau + 0.02);
		const vx = (xb - xa) / 0.04, vy = (yb - ya) / 0.04;
		const speed = Math.sqrt(vx * vx + vy * vy);
		let k = speed / 420.0;
		if (k > 1.0)
			k = 1.0;
		let ux = 1.0, uy = 0.0;
		if (speed > 1.0) {
			ux = vx / speed;
			uy = vy / speed;
		}
		const b = [];
		b.push(ball(x, y, 74.0 + 5.0 * Math.sin(2.1 * t), ux, uy, 1.0 + 0.30 * k, 1.0 - 0.16 * k));
		for (let i = 1; i <= 4; i++) {
			let [tx, ty] = path(tau - 0.16 * i);
			tx += 8.0 * Math.sin(1.7 * t + i * 1.9);
			ty += 8.0 * Math.cos(1.3 * t + i * 2.3);
			b.push(ball(tx, ty, trailR[i - 1], 1.0, 0.0, 1.0, 1.0));
		}
		for (let i = 0; i < 2; i++) {
			const a = 1.9 * t + i * 3.3;
			const orbit = 86.0 + 34.0 * Math.sin(1.3 * t + i * 2.0);
			b.push(ball(x + orbit * Math.cos(a), y + orbit * Math.sin(a) * 0.8, i ? 14.0 : 18.0, 1.0, 0.0, 1.0, 1.0));
		}
		return b;
	}

	function markTiles(b) {
		const rmax = b.r * (b.sa > b.sb ? b.sa : b.sb) + 4.0;
		const x0 = clampi(Math.floor((b.x - rmax) / TILE), 0, TILES_X - 1);
		const x1 = clampi(Math.floor((b.x + rmax) / TILE), 0, TILES_X - 1);
		const y0 = clampi(Math.floor((b.y - rmax) / TILE), 0, TILES_Y - 1);
		const y1 = clampi(Math.floor((b.y + rmax) / TILE), 0, TILES_Y - 1);
		if (b.x + rmax < 0 || b.y + rmax < 0 || b.x - rmax >= SCREEN_W || b.y - rmax >= SCREEN_H)
			return;
		for (let ty = y0; ty <= y1; ty++)
			for (let tx = x0; tx <= x1; tx++)
				tilesNow[ty * TILES_X + tx] = 1;
	}

	function splat(b) {
		const rmax = b.r * (b.sa > b.sb ? b.sa : b.sb);
		let x0 = Math.floor((b.x - rmax) * 0.5), x1 = Math.ceil((b.x + rmax) * 0.5);
		let y0 = Math.floor((b.y - rmax) * 0.5), y1 = Math.ceil((b.y + rmax) * 0.5);
		if (x0 < 0)
			x0 = 0;
		if (y0 < 0)
			y0 = 0;
		if (x1 > GRID_W - 1)
			x1 = GRID_W - 1;
		if (y1 > GRID_H - 1)
			y1 = GRID_H - 1;
		const ra = b.r * b.sa, rb = b.r * b.sb;
		const ia = 1.0 / (ra * ra), ib = 1.0 / (rb * rb);
		for (let j = y0; j <= y1; j++) {
			const dy = (2 * j + 1) - b.y;
			let row = CELL(x0, j);
			for (let i = x0; i <= x1; i++, row++) {
				const dx = (2 * i + 1) - b.x;
				const u = dx * b.ux + dy * b.uy, w = dy * b.ux - dx * b.uy;
				const q = 1.0 - (u * u * ia + w * w * ib);
				if (q > 0.0) {
					if (debug) {
						check(row, GRID_SIZE, 'splat');
						// every cell written must be under a marked tile, or it's never cleared
						if (!tilesNow[(j >> 3) * TILES_X + (i >> 3)])
							throw new Error('splat outside the marked tiles at cell ' + i + ',' + j);
					}
					field[row] += q * q * 4096.0;
				}
			}
		}
	}

	// ---- lighting, at the grid's resolution ----

	function lightTile(tx, ty) {
		for (let j = ty * TILE_CELLS; j < (ty + 1) * TILE_CELLS; j++) {
			let idx = CELL(tx * TILE_CELLS, j);
			for (let i = 0; i < TILE_CELLS; i++, idx++) {
				if (debug) {
					check(idx - STRIDE, GRID_SIZE, 'light');
					check(idx + STRIDE, GRID_SIZE, 'light');
				}
				const f = field[idx];
				if (f <= 0.0) {
					fc[idx] = 0;
					ls[idx] = 0;
					continue;
				}
				stats.cells++;
				const gx = field[idx + 1] - field[idx - 1];
				const gy = field[idx + STRIDE] - field[idx - STRIDE];
				const mag = Math.sqrt(gx * gx + gy * gy);
				const far = (f - F_T) * 96.0;
				const c = far >= 127.0 * mag ? 255 : far <= -127.0 * mag ? 1 : clampi(128 + ((far / mag) | 0), 1, 255);
				const fi = f > 65535.0 ? 65535 : f | 0;
				fc[idx] = (fi | (c << 20)) >>> 0;
				if (f < F_LIT) {
					ls[idx] = 0;
					continue;
				}
				const fa = f + HEIGHT_A;
				const k = SLOPE_K * HEIGHT_A * 0.25 / (fa * fa);
				const tx = gx * k, ty = gy * k;
				const nz = 1.0 / Math.sqrt(1.0 + tx * tx + ty * ty);
				let diffuse = (tx * 0.45 + ty * 0.55 + 0.704) * nz;
				if (diffuse < 0.0)
					diffuse = 0.0;
				let half = (tx * 0.244 + ty * 0.298 + 0.923) * nz;
				if (half < 0.0)
					half = 0.0;
				const h4 = half * half * half * half, h16 = h4 * h4 * h4 * h4;
				const spec = (h16 * h16 * h16 * 255.0) | 0;
				const rim = (1.0 - nz) * (1.0 - nz);
				let light = (LIGHT_AMBIENT + diffuse * LIGHT_DIFFUSE + rim * LIGHT_RIM) | 0;
				if (light > 255)
					light = 255;
				ls[idx] = (light | (spec << 16)) >>> 0;
			}
		}
	}

	function lightRows(part, parts) {
		for (let ty = part; ty < TILES_Y; ty += parts)
			for (let tx = 0; tx < TILES_X; tx++)
				if (tilesNow[ty * TILES_X + tx])
					lightTile(tx, ty);
	}

	// ---- pixels ----

	function shade(fcv, lsv, base, over) {
		const f = (fcv & 0xFFFFF) >> 4, c = fcv >>> 24;
		let r = base & 0xFF, g = (base >> 8) & 0xFF, b = (base >> 16) & 0xFF;
		let glow = f >= F_T ? 256 : f <= F_GLOW ? 0 : ((f - F_GLOW) * 68) >> 8;
		glow = (glow * glow) >> 8;
		r += (glowR * glow) >> 9;
		g += (glowG * glow) >> 9;
		b += (glowB * glow) >> 9;
		let cover = 128 + (c - 128) * 8;
		if (cover > 0) {
			if (cover > 256)
				cover = 256;
			const light = (lsv >> 4) & 0xFF, spec = (lsv >> 20) & 0xFF;
			const col = ramp[f >> 5];
			const lr = (((col & 0xFF) * light) >> 7) + spec;
			const lg = ((((col >> 8) & 0xFF) * light) >> 7) + spec;
			const lb = ((((col >> 16) & 0xFF) * light) >> 7) + spec;
			r += ((lr - r) * cover) >> 8;
			g += ((lg - g) * cover) >> 8;
			b += ((lb - b) * cover) >> 8;
		}
		const oa = over >>> 24;
		const keep = 256 - oa - (oa >> 7);
		r = (over & 0xFF) + ((r * keep) >> 8);
		g = ((over >> 8) & 0xFF) + ((g * keep) >> 8);
		b = ((over >> 16) & 0xFF) + ((b * keep) >> 8);
		if (r > 255)
			r = 255;
		if (g > 255)
			g = 255;
		if (b > 255)
			b = 255;
		return (0xFF000000 | (b << 16) | (g << 8) | r) >>> 0;
	}

	function copyTile(tx, ty) {
		const scene = jobLayers.scene;
		let p = ty * TILE * SCREEN_W + tx * TILE;
		for (let y = 0; y < TILE; y++, p += SCREEN_W)
			jobFb.set(scene.subarray(p, p + TILE), p);
		stats.copied += TILE * TILE;
	}

	function shadeTile(tx, ty) {
		const base = jobLayers.base, over = jobLayers.overlay, scene = jobLayers.scene;
		for (let j = ty * TILE_CELLS; j < (ty + 1) * TILE_CELLS; j++) {
			const m = CELL(tx * TILE_CELLS, j), up = m - STRIDE, dn = m + STRIDE;
			let p = 2 * j * SCREEN_W + tx * TILE;
			for (let i = 0; i < TILE_CELLS; i++, p += 2) {
				if (debug) {
					check(up + i - 1, GRID_SIZE, 'shade');
					check(dn + i + 1, GRID_SIZE, 'shade');
					check(p + SCREEN_W + 1, SCREEN_W * SCREEN_H, 'pixel');
				}
				const c = fc[m + i], l = fc[m + i - 1], r = fc[m + i + 1], u = fc[up + i], d = fc[dn + i];
				const ul = fc[up + i - 1], ur = fc[up + i + 1], dl = fc[dn + i - 1], dr = fc[dn + i + 1];
				if (((c | l | r | u | d | ul | ur | dl | dr) & 0xFFFF) < F_GLOW) {
					jobFb[p] = scene[p];
					jobFb[p + 1] = scene[p + 1];
					jobFb[p + SCREEN_W] = scene[p + SCREEN_W];
					jobFb[p + SCREEN_W + 1] = scene[p + SCREEN_W + 1];
					continue;
				}
				stats.shaded += 4;
				const c9 = c * 9, l3 = l * 3, r3 = r * 3, u3 = u * 3, d3 = d * 3;
				const lc = ls[m + i] * 9, ll = ls[m + i - 1] * 3, lr = ls[m + i + 1] * 3;
				const lup = ls[up + i] * 3, ldn = ls[dn + i] * 3;
				if (debug && (c9 + r3 + d3 + dr >= 4294967296 || c9 + l3 + u3 + ul >= 4294967296))
					throw new Error('packed field sum overflows 32 bits');
				jobFb[p] = shade(c9 + l3 + u3 + ul, lc + ll + lup + ls[up + i - 1], base[p], over[p]);
				jobFb[p + 1] = shade(c9 + r3 + u3 + ur, lc + lr + lup + ls[up + i + 1], base[p + 1], over[p + 1]);
				jobFb[p + SCREEN_W] =
					shade(c9 + l3 + d3 + dl, lc + ll + ldn + ls[dn + i - 1], base[p + SCREEN_W], over[p + SCREEN_W]);
				jobFb[p + SCREEN_W + 1] = shade(c9 + r3 + d3 + dr, lc + lr + ldn + ls[dn + i + 1],
				                                base[p + SCREEN_W + 1], over[p + SCREEN_W + 1]);
			}
		}
	}

	function pixelRows(part, parts) {
		for (let ty = part; ty < TILES_Y; ty += parts)
			for (let tx = 0; tx < TILES_X; tx++) {
				const t = ty * TILES_X + tx;
				if (tilesNow[t]) {
					shadeTile(tx, ty);
					stats.tiles++;
				} else if (jobFull || jobPrev[t]) {
					copyTile(tx, ty);
				}
			}
	}

	// ---- the colours ----

	function buildRamp(t) {
		const pos = t / PALETTE_SECONDS;
		const from = Math.floor(pos % 4.0);
		let k = pos - Math.floor(pos);
		k = k * k * (3.0 - 2.0 * k);
		const to = (from + 1) & 3;
		const stops = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
		for (let s = 0; s < 3; s++)
			for (let ch = 0; ch < 3; ch++)
				stops[s][ch] = (PALETTES[from][s][ch] + (PALETTES[to][s][ch] - PALETTES[from][s][ch]) * k) | 0;
		for (let i = 0; i < RAMP_N; i++) {
			const over = i * 32 + 16 - F_T;
			const h = over <= 0.0 ? 0.0 : over / (over + 2600.0);
			const a = h < 0.5 ? 0 : 1;
			const w = h < 0.5 ? h * 2.0 : h * 2.0 - 1.0;
			const r = stops[a][0] + (((stops[a + 1][0] - stops[a][0]) * w) | 0);
			const g = stops[a][1] + (((stops[a + 1][1] - stops[a][1]) * w) | 0);
			const b = stops[a][2] + (((stops[a + 1][2] - stops[a][2]) * w) | 0);
			ramp[i] = (r | (g << 8) | (b << 16)) >>> 0;
		}
		glowR = stops[1][0];
		glowG = stops[1][1];
		glowB = stops[1][2];
	}

	// ---- the frame ----

	function clearTile(tx, ty) {
		for (let j = ty * TILE_CELLS; j < (ty + 1) * TILE_CELLS; j++) {
			const idx = CELL(tx * TILE_CELLS, j);
			field.fill(0, idx, idx + TILE_CELLS);
			fc.fill(0, idx, idx + TILE_CELLS);
			ls.fill(0, idx, idx + TILE_CELLS);
		}
	}

	// still: balls to draw in place of the moving ones, for artwork (not in liquid.c)
	function frame(fb, buf, t, full, layers, still) {
		for (let ty = 0; ty < TILES_Y; ty++)
			for (let tx = 0; tx < TILES_X; tx++)
				if (tilesField[ty * TILES_X + tx])
					clearTile(tx, ty);
		tilesNow.fill(0);
		const balls = still ? still.map((b) => ball(b[0], b[1], b[2], 1.0, 0.0, 1.0, 1.0)) : placeBalls(t);
		for (const b of balls)
			markTiles(b);
		for (const b of balls)
			splat(b);
		buildRamp(t);

		jobFb = fb;
		jobPrev = tilesPrev[buf];
		jobFull = full;
		jobLayers = layers;
		stats.cells = stats.shaded = stats.copied = stats.tiles = 0;
		lightRows(0, 1);
		pixelRows(0, 1);

		tilesPrev[buf].set(tilesNow);
		tilesField.set(tilesNow);
		return stats;
	}

	return { frame, placeBalls };
}

if (typeof module !== 'undefined')
	module.exports = { createLiquid, SCREEN_W, SCREEN_H };
