#include "sha256.h"

#include <string.h>

static const uint32_t K[64] = {
	0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
	0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
	0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
	0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
	0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
	0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
	0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
	0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

static inline uint32_t rotr(uint32_t x, int n) {
	return (x >> n) | (x << (32 - n));
}

static void block(uint32_t h[8], const uint8_t *p) {
	uint32_t w[64];
	for (int i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
		       (uint32_t)p[i * 4 + 3];
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t t1 = hh + s1 + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

void sha256_begin(Sha256 *s) {
	static const uint32_t start[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
	                                  0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
	memcpy(s->h, start, sizeof(start));
	s->held = 0;
	s->total = 0;
}

void sha256_add(Sha256 *s, const uint8_t *data, uint32_t len) {
	s->total += len;
	if (s->held) { // fill the block that was left part full
		uint32_t take = 64 - s->held < len ? 64 - s->held : len;
		memcpy(s->part + s->held, data, take);
		s->held += take;
		data += take;
		len -= take;
		if (s->held < 64)
			return;
		block(s->h, s->part);
		s->held = 0;
	}
	for (; len >= 64; data += 64, len -= 64)
		block(s->h, data);
	if (len) {
		memcpy(s->part, data, len);
		s->held = len;
	}
}

void sha256_end(Sha256 *s, char hex[65]) {
	// the rest, a 1 bit, zeros, and the length in bits as 64 bits: one block or two
	uint8_t tail[128];
	uint32_t rest = s->held, tail_len = rest < 56 ? 64 : 128;
	uint64_t bits = s->total * 8;
	memset(tail, 0, sizeof(tail));
	memcpy(tail, s->part, rest);
	tail[rest] = 0x80;
	for (int i = 0; i < 8; i++)
		tail[tail_len - 1 - i] = (uint8_t)(bits >> (8 * i));
	block(s->h, tail);
	if (tail_len == 128)
		block(s->h, tail + 64);
	static const char digits[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) {
		uint32_t byte = (s->h[i >> 2] >> (24 - 8 * (i & 3))) & 0xFF;
		hex[i * 2] = digits[byte >> 4];
		hex[i * 2 + 1] = digits[byte & 15];
	}
	hex[64] = '\0';
}

void sha256_hex(const uint8_t *data, uint32_t len, char hex[65]) {
	Sha256 s;
	sha256_begin(&s);
	sha256_add(&s, data, len);
	sha256_end(&s, hex);
}
