// SHA-1 (FIPS 180-4), which the digests inside a package header are made of
// (headbin.h). Nothing here relies on it for safety: a download is checked
// with SHA-256.
#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>

void sha1(const uint8_t *data, uint32_t len, uint8_t out[20]);

#endif
