// SHA-256 (FIPS 180-4), for checking a download against the digest GitHub
// gives for it.
#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

// The digest of data as 64 lower-case hex digits and a NUL.
void sha256_hex(const uint8_t *data, uint32_t len, char hex[65]);

// The same for data that arrives in pieces: begin, add each piece, end.
typedef struct {
	uint32_t h[8];
	uint8_t part[64]; // a block not yet full
	uint32_t held;    // how much of it is
	uint64_t total;   // bytes so far
} Sha256;
void sha256_begin(Sha256 *s);
void sha256_add(Sha256 *s, const uint8_t *data, uint32_t len);
void sha256_end(Sha256 *s, char hex[65]);

#endif
