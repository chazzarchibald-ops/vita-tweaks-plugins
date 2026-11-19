// Reads a zip (a VPK is one): its list of files, and each file's contents,
// stored or deflated, checked against the CRC the zip gives. A zip can be held
// whole in memory, or read a piece at a time and unpacked a piece at a time,
// for one that is bigger than memory.
#ifndef UNZIP_H
#define UNZIP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	const char *name; // inside the zip: not NUL-terminated
	int name_len;
	uint32_t crc, packed, size;
	uint32_t offset; // of its local header
	int method;      // 0 stored, 8 deflated
} ZipEntry;

// ---- a zip in memory ----

// The zip's files, up to max of them. Returns how many, or -1 if this isn't a
// zip this code reads (zip64, encrypted, split or damaged).
int zip_entries(const uint8_t *zip, uint32_t len, ZipEntry *out, int max);

// One file's contents: malloc'd, entry->size bytes (a byte more is allocated,
// so an empty file isn't NULL). NULL if it doesn't unpack to its size and CRC.
uint8_t *zip_extract(const uint8_t *zip, uint32_t len, const ZipEntry *entry);

// ---- a zip read a piece at a time ----

// Reads len bytes of the zip from offset. Returns how many it read.
typedef int (*ZipRead)(void *from, uint32_t offset, uint8_t *buf, uint32_t len);
// Takes the next len bytes of a file being unpacked. False if it couldn't.
typedef bool (*ZipWrite)(void *to, const uint8_t *buf, uint32_t len);

// As zip_entries, for a zip of len bytes. The names point into *names, which
// is malloc'd: free it when the entries are done with.
int zip_file_entries(ZipRead read, void *from, uint32_t len, ZipEntry *out, int max, uint8_t **names);

// One file's contents, handed to write up to 64 KB at a time. False if it
// doesn't unpack to its size and CRC, or write refused.
bool zip_file_extract(ZipRead read, void *from, uint32_t len, const ZipEntry *entry, ZipWrite write, void *to);

// ---- on their own ----

// Unpacks deflated data (RFC 1951, nothing around it) to exactly out_len bytes
// of out. False if it doesn't unpack to that.
bool zip_inflate(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_len);

uint32_t zip_crc32(const uint8_t *data, uint32_t len);
// The CRC of data that comes in pieces: 0 to begin with, then each result.
uint32_t zip_crc32_add(uint32_t crc, const uint8_t *data, uint32_t len);

#endif
