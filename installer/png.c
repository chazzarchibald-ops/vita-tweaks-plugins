#include "png.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "unzip.h"

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline int paeth(int a, int b, int c) {
	int p = a + b - c;
	int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// Undoes a row's filter in place. prev is the row above, already undone, or NULL for the first.
static bool unfilter(int type, uint8_t *row, const uint8_t *prev, uint32_t n, uint32_t bpp) {
	for (uint32_t i = 0; i < n; i++) {
		int left = i >= bpp ? row[i - bpp] : 0, up = prev ? prev[i] : 0, corner = prev && i >= bpp ? prev[i - bpp] : 0;
		int add;
		switch (type) {
		case 0: add = 0; break;
		case 1: add = left; break;
		case 2: add = up; break;
		case 3: add = (left + up) >> 1; break;
		case 4: add = paeth(left, up, corner); break;
		default: return false;
		}
		row[i] = (uint8_t)(row[i] + add);
	}
	return true;
}

uint8_t *png_decode(const uint8_t *png, uint32_t len, uint32_t max_pixels, int *w, int *h) {
	static const uint8_t magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	if (len < 8 + 25 || memcmp(png, magic, 8) != 0)
		return NULL;
	uint32_t width = 0, height = 0, type = 0, channels = 0, palette_len = 0, packed_len = 0;
	const uint8_t *palette = NULL;
	uint8_t *packed = malloc(len); // every IDAT chunk's data, end to end: less than the file
	uint8_t *raw = NULL, *out = NULL;
	bool ok = packed != NULL, ended = false;
	// the chunks: length, name, data, CRC (which isn't checked: the file's own checksum is)
	for (uint32_t at = 8; ok && !ended;) {
		if (len - at < 12) {
			ok = false;
			break;
		}
		uint32_t size = be32(png + at);
		const uint8_t *name = png + at + 4, *data = png + at + 8;
		if (size > len - at - 12) {
			ok = false;
			break;
		}
		if (memcmp(name, "IHDR", 4) == 0) {
			if (size < 13 || width) {
				ok = false;
				break;
			}
			width = be32(data);
			height = be32(data + 4);
			type = data[9];
			// 8 bits a channel, the usual compression and filters, not interlaced
			channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
			ok = data[8] == 8 && channels && !data[10] && !data[11] && !data[12] && width && height &&
			     width <= 8192 && height <= 8192 && width * height <= max_pixels;
		} else if (memcmp(name, "PLTE", 4) == 0) {
			palette = data;
			palette_len = size / 3;
		} else if (memcmp(name, "IDAT", 4) == 0) {
			memcpy(packed + packed_len, data, size);
			packed_len += size;
		} else if (memcmp(name, "IEND", 4) == 0) {
			ended = true;
		}
		at += 12 + size;
	}
	ok = ok && ended && width && packed_len > 6 && (type != 3 || palette_len);
	uint32_t row_len = width * channels, raw_len = (row_len + 1) * height;
	if (ok) {
		// a zlib stream: two bytes that say "deflate", the data, and a checksum
		raw = malloc(raw_len);
		ok = raw && (packed[0] & 0x0F) == 8 && !(packed[1] & 0x20) &&
		     zip_inflate(packed + 2, packed_len - 2, raw, raw_len);
	}
	if (ok) { // that checksum (Adler-32), which a damaged picture won't match
		uint32_t a = 1, b = 0;
		for (uint32_t i = 0; i < raw_len;) {
			uint32_t stop = raw_len - i > 5000 ? i + 5000 : raw_len; // (the sums are reduced before they can overflow)
			for (; i < stop; i++) {
				a += raw[i];
				b += a;
			}
			a %= 65521;
			b %= 65521;
		}
		ok = be32(packed + packed_len - 4) == ((b << 16) | a);
	}
	if (ok)
		out = malloc((size_t)width * height * 3);
	ok = ok && out;
	for (uint32_t y = 0; ok && y < height; y++) {
		uint8_t *row = raw + y * (row_len + 1) + 1;
		ok = unfilter(row[-1], row, y ? row - (row_len + 1) : NULL, row_len, channels);
		uint8_t *to = out + (size_t)y * width * 3;
		for (uint32_t x = 0; ok && x < width; x++, to += 3) {
			const uint8_t *px = row + x * channels;
			if (type == 2 || type == 6) {
				to[0] = px[0];
				to[1] = px[1];
				to[2] = px[2];
			} else if (type == 3) {
				if (px[0] >= palette_len) {
					ok = false;
					break;
				}
				memcpy(to, palette + px[0] * 3, 3);
			} else {
				to[0] = to[1] = to[2] = px[0];
			}
		}
	}
	free(packed);
	free(raw);
	if (!ok) {
		free(out);
		return NULL;
	}
	*w = (int)width;
	*h = (int)height;
	return out;
}

uint8_t *photo_fit(const uint8_t *rgb, int w, int h, int fw, int fh, uint32_t fill) {
	uint8_t *out = malloc((size_t)fw * fh * 3);
	if (!out || w <= 0 || h <= 0 || w > 8192 || h > 8192 || fw > 4096 || fh > 4096)
		return out;
	for (int i = 0; i < fw * fh; i++) {
		out[i * 3] = (uint8_t)fill;
		out[i * 3 + 1] = (uint8_t)(fill >> 8);
		out[i * 3 + 2] = (uint8_t)(fill >> 16);
	}
	// the biggest size of its shape that fits, in whole pixels
	// (whole-number sums that fit in 32 bits for any picture png_decode gives)
	int ow = fw, oh = (h * fw + w / 2) / w;
	if (oh > fh) {
		oh = fh;
		ow = (w * fh + h / 2) / h;
	}
	if (ow < 1)
		ow = 1;
	if (oh < 1)
		oh = 1;
	int ox = (fw - ow) / 2, oy = (fh - oh) / 2;
	for (int y = 0; y < oh; y++) {
		// the rows of the picture this row covers: at least one
		int y0 = y * h / oh, y1 = (y + 1) * h / oh;
		if (y1 <= y0)
			y1 = y0 + 1;
		uint8_t *to = out + ((size_t)(oy + y) * fw + ox) * 3;
		for (int x = 0; x < ow; x++, to += 3) {
			int x0 = x * w / ow, x1 = (x + 1) * w / ow;
			if (x1 <= x0)
				x1 = x0 + 1;
			uint32_t sum[3] = {0, 0, 0}, n = (uint32_t)((x1 - x0) * (y1 - y0));
			for (int sy = y0; sy < y1; sy++) {
				const uint8_t *from = rgb + ((size_t)sy * w + x0) * 3;
				for (int sx = x0; sx < x1; sx++, from += 3) {
					sum[0] += from[0];
					sum[1] += from[1];
					sum[2] += from[2];
				}
			}
			to[0] = (uint8_t)((sum[0] + n / 2) / n);
			to[1] = (uint8_t)((sum[1] + n / 2) / n);
			to[2] = (uint8_t)((sum[2] + n / 2) / n);
		}
	}
	return out;
}
