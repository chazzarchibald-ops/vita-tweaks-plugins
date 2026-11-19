#include "sha1.h"

#include <string.h>

static inline uint32_t rotl(uint32_t x, int n) {
	return (x << n) | (x >> (32 - n));
}

static void block(uint32_t h[5], const uint8_t *p) {
	uint32_t w[80];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
		       (uint32_t)p[i * 4 + 3];
	for (int i = 16; i < 80; i++)
		w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		uint32_t t = rotl(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rotl(b, 30);
		b = a;
		a = t;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
}

void sha1(const uint8_t *data, uint32_t len, uint8_t out[20]) {
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	uint32_t whole = len / 64 * 64;
	for (uint32_t at = 0; at < whole; at += 64)
		block(h, data + at);
	// what is left, the 0x80 that ends the message, and its length in bits
	uint8_t tail[128];
	uint32_t rest = len - whole, size = rest + 9 <= 64 ? 64 : 128;
	memset(tail, 0, sizeof(tail));
	if (rest)
		memcpy(tail, data + whole, rest);
	tail[rest] = 0x80;
	tail[size - 5] = (uint8_t)(len >> 29);
	tail[size - 4] = (uint8_t)(len >> 21);
	tail[size - 3] = (uint8_t)(len >> 13);
	tail[size - 2] = (uint8_t)(len >> 5);
	tail[size - 1] = (uint8_t)(len << 3);
	block(h, tail);
	if (size == 128)
		block(h, tail + 64);
	for (int i = 0; i < 5; i++) {
		out[i * 4] = (uint8_t)(h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)h[i];
	}
}
