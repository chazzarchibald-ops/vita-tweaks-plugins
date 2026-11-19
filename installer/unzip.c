#include "unzip.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t u16(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t u32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t zip_crc32_add(uint32_t crc, const uint8_t *data, uint32_t len) {
	static uint32_t table[256];
	if (!table[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	crc ^= 0xFFFFFFFFu;
	for (uint32_t i = 0; i < len; i++)
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

uint32_t zip_crc32(const uint8_t *data, uint32_t len) {
	return zip_crc32_add(0, data, len);
}

// ---- the list of files ----

#define END_SIG 0x06054B50u
#define CENTRAL_SIG 0x02014B50u
#define LOCAL_SIG 0x04034B50u
#define END_SIZE 22
#define CENTRAL_SIZE 46
#define LOCAL_SIZE 30
#define COMMENT_MAX 0xFFFF

// Where the end record is in the last tail_len bytes of a zip, or -1. It is
// the last thing in the file, but for a comment of up to 64 KB.
static int find_end(const uint8_t *tail, uint32_t tail_len) {
	if (tail_len < END_SIZE)
		return -1;
	for (uint32_t at = tail_len - END_SIZE;; at--) {
		if (u32(tail + at) == END_SIG)
			return (int)at;
		if (at == 0)
			return -1;
	}
}

// What the end record says of the directory. False for a zip in several parts, or a zip64.
static bool read_end(const uint8_t *end, uint32_t *count, uint32_t *dir_size, uint32_t *dir) {
	*count = u16(end + 10);
	*dir_size = u32(end + 12);
	*dir = u32(end + 16);
	if (u16(end + 4) || u16(end + 6) || *count != u16(end + 8))
		return false;
	return *count != 0xFFFF && *dir != 0xFFFFFFFFu;
}

// The directory's entries, their names pointing into dir.
static int read_directory(const uint8_t *dir, uint32_t dir_size, uint32_t count, ZipEntry *out, int max) {
	int n = 0;
	uint32_t at = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (at > dir_size || dir_size - at < CENTRAL_SIZE || u32(dir + at) != CENTRAL_SIG)
			return -1;
		uint32_t flags = u16(dir + at + 8), name_len = u16(dir + at + 28);
		uint32_t skip = name_len + u16(dir + at + 30) + u16(dir + at + 32);
		if (dir_size - at - CENTRAL_SIZE < skip)
			return -1;
		if (flags & 1)
			return -1; // encrypted
		if (n < max) {
			ZipEntry *e = &out[n];
			e->name = (const char *)dir + at + CENTRAL_SIZE;
			e->name_len = (int)name_len;
			e->method = (int)u16(dir + at + 10);
			e->crc = u32(dir + at + 16);
			e->packed = u32(dir + at + 20);
			e->size = u32(dir + at + 24);
			e->offset = u32(dir + at + 42);
			if (e->packed == 0xFFFFFFFFu || e->size == 0xFFFFFFFFu || e->offset == 0xFFFFFFFFu)
				return -1; // zip64
			n++;
		}
		at += CENTRAL_SIZE + skip;
	}
	return n;
}

int zip_entries(const uint8_t *zip, uint32_t len, ZipEntry *out, int max) {
	uint32_t tail_len = len > END_SIZE + COMMENT_MAX ? END_SIZE + COMMENT_MAX : len;
	int found = find_end(zip + len - tail_len, tail_len);
	if (found < 0)
		return -1;
	uint32_t end = len - tail_len + (uint32_t)found, count, dir_size, dir;
	if (!read_end(zip + end, &count, &dir_size, &dir) || dir > end || dir_size > end - dir)
		return -1; // a directory that isn't where it says
	return read_directory(zip + dir, dir_size, count, out, max);
}

int zip_file_entries(ZipRead read, void *from, uint32_t len, ZipEntry *out, int max, uint8_t **names) {
	*names = NULL;
	uint32_t tail_len = len > END_SIZE + COMMENT_MAX ? END_SIZE + COMMENT_MAX : len;
	uint8_t *tail = malloc(tail_len + 1);
	if (!tail)
		return -1;
	int found = read(from, len - tail_len, tail, tail_len) == (int)tail_len ? find_end(tail, tail_len) : -1;
	uint32_t end = len - tail_len + (uint32_t)(found < 0 ? 0 : found), count = 0, dir_size = 0, dir = 0;
	bool ok = found >= 0 && read_end(tail + found, &count, &dir_size, &dir) && dir <= end && dir_size <= end - dir;
	free(tail);
	if (!ok)
		return -1;
	uint8_t *buf = malloc(dir_size + 1);
	if (!buf)
		return -1;
	int n = read(from, dir, buf, dir_size) == (int)dir_size ? read_directory(buf, dir_size, count, out, max) : -1;
	if (n < 0) {
		free(buf);
		return -1;
	}
	*names = buf;
	return n;
}

// ---- inflate: RFC 1951 ----
// Codes are read a bit at a time against counts of codes per length, which is
// slow but small. What comes out goes through a 32 KB window, the furthest
// back a copy can reach, and is handed on a windowful at a time, so a file of
// any size unpacks in the same memory.

#define MAX_BITS 15
#define MAX_LITERALS 286
#define MAX_DISTANCES 30
#define WINDOW 32768u
#define IN_BUF (64 * 1024)

typedef struct {
	// what goes in: a buffer, filled again from the zip when it runs out
	const uint8_t *in;
	uint32_t in_len, in_pos;
	ZipRead read; // NULL when all of it is in the buffer already
	void *from;
	uint8_t *in_buf;
	uint32_t file_at, file_left;
	uint32_t bit_buf;
	int bit_count;
	// what comes out
	uint8_t *window;
	uint32_t out_len, out_pos;
	ZipWrite write;
	void *to;
	uint32_t crc;
	bool bad; // ran out of input, the data makes no sense, or it couldn't be written
} Inflate;

typedef struct {
	uint16_t count[MAX_BITS + 1]; // codes of each length
	uint16_t symbol[MAX_LITERALS + 2]; // symbols in order of code
} Huffman;

static bool refill(Inflate *s) {
	if (!s->read || !s->file_left)
		return false;
	uint32_t want = s->file_left < IN_BUF ? s->file_left : IN_BUF;
	if (s->read(s->from, s->file_at, s->in_buf, want) != (int)want)
		return false;
	s->file_at += want;
	s->file_left -= want;
	s->in = s->in_buf;
	s->in_len = want;
	s->in_pos = 0;
	return true;
}

static inline uint32_t next_byte(Inflate *s) {
	if (s->in_pos >= s->in_len && !refill(s)) {
		s->bad = true;
		return 0;
	}
	return s->in[s->in_pos++];
}

static void hand_on(Inflate *s, uint32_t n) {
	s->crc = zip_crc32_add(s->crc, s->window, n);
	if (n && !s->write(s->to, s->window, n))
		s->bad = true;
}

static inline void emit(Inflate *s, uint8_t byte) {
	if (s->out_pos >= s->out_len) {
		s->bad = true;
		return;
	}
	s->window[s->out_pos & (WINDOW - 1)] = byte;
	s->out_pos++;
	if (!(s->out_pos & (WINDOW - 1)))
		hand_on(s, WINDOW);
}

static uint32_t bits(Inflate *s, int need) {
	uint32_t value = s->bit_buf;
	while (s->bit_count < need) {
		uint32_t byte = next_byte(s);
		if (s->bad)
			return 0;
		value |= byte << s->bit_count;
		s->bit_count += 8;
	}
	s->bit_buf = value >> need;
	s->bit_count -= need;
	return value & ((1u << need) - 1u);
}

static int decode(Inflate *s, const Huffman *h) {
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= MAX_BITS; len++) {
		code |= (int)bits(s, 1);
		if (s->bad)
			return -1;
		int count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	s->bad = true;
	return -1;
}

// Builds the table from each symbol's code length. Returns 0 for a complete
// set of codes, more for an incomplete one, less for one that can't be.
static int construct(Huffman *h, const uint8_t *length, int n) {
	uint16_t offset[MAX_BITS + 1];
	for (int len = 0; len <= MAX_BITS; len++)
		h->count[len] = 0;
	for (int sym = 0; sym < n; sym++)
		h->count[length[sym]]++;
	if (h->count[0] == n)
		return 0; // no codes at all
	int left = 1;
	for (int len = 1; len <= MAX_BITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offset[1] = 0;
	for (int len = 1; len < MAX_BITS; len++)
		offset[len + 1] = (uint16_t)(offset[len] + h->count[len]);
	for (int sym = 0; sym < n; sym++)
		if (length[sym])
			h->symbol[offset[length[sym]]++] = (uint16_t)sym;
	return left;
}

static void codes(Inflate *s, const Huffman *len_code, const Huffman *dist_code) {
	static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
	                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
	static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
	                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
	                                       33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
	                                       1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
	                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
	for (;;) {
		int sym = decode(s, len_code);
		if (s->bad)
			return;
		if (sym < 256) {
			emit(s, (uint8_t)sym);
			if (s->bad)
				return;
			continue;
		}
		if (sym == 256)
			return; // the end of the block
		sym -= 257;
		if (sym >= 29) {
			s->bad = true;
			return;
		}
		uint32_t len = len_base[sym] + bits(s, len_extra[sym]);
		sym = decode(s, dist_code);
		if (s->bad)
			return;
		if (sym >= 30) {
			s->bad = true;
			return;
		}
		uint32_t dist = dist_base[sym] + bits(s, dist_extra[sym]);
		if (s->bad || dist > s->out_pos || len > s->out_len - s->out_pos) {
			s->bad = true;
			return;
		}
		// a byte at a time: the copy may run into what it is writing
		for (uint32_t i = 0; i < len && !s->bad; i++)
			emit(s, s->window[(s->out_pos - dist) & (WINDOW - 1)]);
		if (s->bad)
			return;
	}
}

static void fixed_block(Inflate *s) {
	// (built each time: such blocks are rare, and nothing is shared between
	// two threads unpacking at once)
	Huffman len_code, dist_code;
	uint8_t length[288];
	int sym = 0;
	for (; sym < 144; sym++)
		length[sym] = 8;
	for (; sym < 256; sym++)
		length[sym] = 9;
	for (; sym < 280; sym++)
		length[sym] = 7;
	for (; sym < 288; sym++)
		length[sym] = 8;
	construct(&len_code, length, 288);
	for (sym = 0; sym < MAX_DISTANCES; sym++)
		length[sym] = 5;
	construct(&dist_code, length, MAX_DISTANCES);
	codes(s, &len_code, &dist_code);
}

static void dynamic_block(Inflate *s) {
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	uint8_t length[MAX_LITERALS + MAX_DISTANCES];
	Huffman len_code, dist_code;
	int n_len = (int)bits(s, 5) + 257, n_dist = (int)bits(s, 5) + 1, n_code = (int)bits(s, 4) + 4;
	if (s->bad || n_len > MAX_LITERALS || n_dist > MAX_DISTANCES) {
		s->bad = true;
		return;
	}
	// the code that the code lengths themselves are written in
	int i = 0;
	for (; i < n_code; i++)
		length[order[i]] = (uint8_t)bits(s, 3);
	for (; i < 19; i++)
		length[order[i]] = 0;
	if (s->bad || construct(&len_code, length, 19) != 0) {
		s->bad = true;
		return;
	}
	i = 0;
	while (i < n_len + n_dist) {
		int sym = decode(s, &len_code);
		if (s->bad)
			return;
		if (sym < 16) {
			length[i++] = (uint8_t)sym;
			continue;
		}
		uint8_t value = 0;
		int repeat;
		if (sym == 16) {
			if (i == 0) {
				s->bad = true;
				return;
			}
			value = length[i - 1];
			repeat = 3 + (int)bits(s, 2);
		} else if (sym == 17) {
			repeat = 3 + (int)bits(s, 3);
		} else {
			repeat = 11 + (int)bits(s, 7);
		}
		if (s->bad || i + repeat > n_len + n_dist) {
			s->bad = true;
			return;
		}
		while (repeat--)
			length[i++] = value;
	}
	if (length[256] == 0) { // no way to end the block
		s->bad = true;
		return;
	}
	// An incomplete set is only allowed when it is a single code.
	int left = construct(&len_code, length, n_len);
	if (left < 0 || (left > 0 && n_len - len_code.count[0] != 1)) {
		s->bad = true;
		return;
	}
	left = construct(&dist_code, length + n_len, n_dist);
	if (left < 0 || (left > 0 && n_dist - dist_code.count[0] != 1)) {
		s->bad = true;
		return;
	}
	codes(s, &len_code, &dist_code);
}

static void stored_block(Inflate *s) {
	s->bit_buf = 0; // what's left of the byte isn't used
	s->bit_count = 0;
	uint32_t len = next_byte(s);
	len |= next_byte(s) << 8;
	uint32_t check = next_byte(s);
	check |= next_byte(s) << 8;
	if (s->bad || len != (~check & 0xFFFFu) || len > s->out_len - s->out_pos) {
		s->bad = true;
		return;
	}
	for (uint32_t i = 0; i < len && !s->bad; i++)
		emit(s, (uint8_t)next_byte(s));
}

// Unpacks what s reads to exactly out_len bytes, handed to its write. True
// if that worked; s->crc is then the CRC of what came out.
static bool inflate(Inflate *s) {
	s->window = malloc(WINDOW);
	if (!s->window)
		return false;
	uint32_t last;
	do {
		last = bits(s, 1);
		uint32_t type = bits(s, 2);
		if (s->bad)
			break;
		if (type == 0)
			stored_block(s);
		else if (type == 1)
			fixed_block(s);
		else if (type == 2)
			dynamic_block(s);
		else
			s->bad = true;
	} while (!last && !s->bad);
	if (!s->bad)
		hand_on(s, s->out_pos & (WINDOW - 1)); // the last, part-full window
	free(s->window);
	return !s->bad && s->out_pos == s->out_len;
}

// ---- a file, from a zip in memory ----

typedef struct {
	uint8_t *out;
	uint32_t at;
} Grow;

static bool write_memory(void *to, const uint8_t *buf, uint32_t len) {
	Grow *g = to;
	memcpy(g->out + g->at, buf, len);
	g->at += len;
	return true;
}

bool zip_inflate(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len) {
	Grow grow = {out, 0};
	Inflate s;
	memset(&s, 0, sizeof(s));
	s.in = in;
	s.in_len = in_len;
	s.out_len = out_len;
	s.write = write_memory;
	s.to = &grow;
	return inflate(&s);
}

uint8_t *zip_extract(const uint8_t *zip, uint32_t len, const ZipEntry *e) {
	if (e->offset > len || len - e->offset < LOCAL_SIZE || u32(zip + e->offset) != LOCAL_SIG)
		return NULL;
	uint32_t skip = LOCAL_SIZE + u16(zip + e->offset + 26) + u16(zip + e->offset + 28);
	if (len - e->offset < skip || len - e->offset - skip < e->packed)
		return NULL;
	const uint8_t *data = zip + e->offset + skip;
	uint8_t *out = malloc((size_t)e->size + 1);
	if (!out)
		return NULL;
	bool ok;
	if (e->method == 0) {
		ok = e->packed == e->size;
		if (ok)
			memcpy(out, data, e->size);
		ok = ok && zip_crc32(out, e->size) == e->crc;
	} else {
		Grow grow = {out, 0};
		Inflate s;
		memset(&s, 0, sizeof(s));
		s.in = data;
		s.in_len = e->packed;
		s.out_len = e->size;
		s.write = write_memory;
		s.to = &grow;
		ok = e->method == 8 && inflate(&s) && s.crc == e->crc;
	}
	if (!ok) {
		free(out);
		return NULL;
	}
	return out;
}

// ---- a file, from a zip read a piece at a time ----

bool zip_file_extract(ZipRead read, void *from, uint32_t len, const ZipEntry *e, ZipWrite write, void *to) {
	uint8_t local[LOCAL_SIZE];
	if (e->offset > len || len - e->offset < LOCAL_SIZE || read(from, e->offset, local, LOCAL_SIZE) != LOCAL_SIZE ||
	    u32(local) != LOCAL_SIG)
		return false;
	uint32_t skip = LOCAL_SIZE + u16(local + 26) + u16(local + 28);
	if (len - e->offset < skip || len - e->offset - skip < e->packed)
		return false;
	uint32_t at = e->offset + skip;
	uint8_t *buf = malloc(IN_BUF);
	if (!buf)
		return false;
	bool ok = false;
	if (e->method == 0) {
		uint32_t crc = 0, left = e->packed == e->size ? e->size : 0;
		ok = e->packed == e->size;
		while (ok && left) {
			uint32_t n = left < IN_BUF ? left : IN_BUF;
			ok = read(from, at, buf, n) == (int)n;
			if (ok) {
				crc = zip_crc32_add(crc, buf, n);
				ok = write(to, buf, n);
			}
			at += n;
			left -= n;
		}
		ok = ok && crc == e->crc;
	} else if (e->method == 8) {
		Inflate s;
		memset(&s, 0, sizeof(s));
		s.read = read;
		s.from = from;
		s.in_buf = buf;
		s.file_at = at;
		s.file_left = e->packed;
		s.out_len = e->size;
		s.write = write;
		s.to = to;
		ok = inflate(&s) && s.crc == e->crc;
	}
	free(buf);
	return ok;
}
