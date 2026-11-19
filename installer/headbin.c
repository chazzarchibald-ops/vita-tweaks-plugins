#include "headbin.h"

#include <string.h>

#include "sha1.h"

#define CONTENT_ID 0x30
#define TITLE_AT (CONTENT_ID + 7) // after "EP9000-"
#define DIGEST 16

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Which byte of the data's SHA-1 goes where in the block that is hashed again.
// The rest of the 64 bytes are zero.
static const uint8_t BLOCK_FROM[32] = {4,  5,  6,  7,  8,  9,  10, 11, 4,  5,  6,  7,  8,  9,  10, 11,
                                       12, 13, 14, 15, 16, 1,  2,  3,  12, 13, 14, 15, 16, 1,  2,  3};

static void digest(const uint8_t *data, uint32_t len, uint8_t out[DIGEST]) {
	uint8_t first[20], block[64], second[20];
	sha1(data, len, first);
	memset(block, 0, sizeof(block));
	for (int i = 0; i < 32; i++)
		block[i] = first[BLOCK_FROM[i]];
	sha1(block, sizeof(block), second);
	memcpy(out, second, DIGEST);
}

// The three stretches that are digested, and where each digest goes.
typedef struct {
	uint32_t from, len, out;
} Part;

static bool parts_of(const uint8_t *head, uint32_t len, Part part[3]) {
	if (len < 0x100 || memcmp(head, "\x7fPKG", 4) != 0 || head[CONTENT_ID + 6] != '-' || head[CONTENT_ID + 16] != '_')
		return false;
	uint32_t header = be32(head + 0xD0), info = be32(head + 0x08), info_len = be32(head + 0x10);
	uint32_t info_out = be32(head + 0xD4), all = be32(head + 0xE8);
	if (header < 0x100 || header > len || DIGEST > len - header || info > len || info_len < 64 ||
	    info_len - 64 > len - info || info_out > len || DIGEST > len - info_out || all > len || DIGEST > len - all)
		return false;
	part[0] = (Part){0, header, header};
	part[1] = (Part){info, info_len - 64, info_out};
	part[2] = (Part){0, all, all}; // last: it covers the other two
	return true;
}

bool headbin_sound(const uint8_t *head, uint32_t len) {
	Part part[3];
	if (!parts_of(head, len, part))
		return false;
	for (int i = 0; i < 3; i++) {
		uint8_t d[DIGEST];
		digest(head + part[i].from, part[i].len, d);
		if (memcmp(d, head + part[i].out, DIGEST) != 0)
			return false;
	}
	return true;
}

void headbin_title(const uint8_t *head, char titleid[10]) {
	memcpy(titleid, head + TITLE_AT, 9);
	titleid[9] = '\0';
}

bool headbin_retitle(uint8_t *head, uint32_t len, const char *titleid) {
	Part part[3];
	if (strlen(titleid) != 9 || !headbin_sound(head, len) || !parts_of(head, len, part))
		return false;
	memcpy(head + TITLE_AT, titleid, 9);
	for (int i = 0; i < 3; i++)
		digest(head + part[i].from, part[i].len, head + part[i].out);
	return true;
}
