// head.bin: the package header the system's installer wants at
// sce_sys/package/head.bin in a folder it is asked to install. Whatever
// installed this app (VitaShell, say) made one for it, and it is still in the
// app's folder. Nothing is shipped here: the app reuses its own header to
// install its next version, and makes the updater's from it by putting the
// updater's title ID in and working out the header's three digests again.
//
// The layout, as far as this needs it (numbers are big-endian):
//   0x00  "\x7fPKG"
//   0x08  where the package's info block starts      0x10  its length
//   0x30  the content ID, "EP9000-TWKS00001_00-0000000000000000"
//   0xD0  length of the header; its digest follows it
//   0xD4  where the info block's digest goes (over the block less its last 64 bytes)
//   0xE8  length of everything before the last digest, which follows it
// A digest is 16 bytes: the SHA-1 of the data, some of its bytes laid out in
// a 64-byte block (see headbin.c), and the SHA-1 of that block.
#ifndef HEADBIN_H
#define HEADBIN_H

#include <stdbool.h>
#include <stdint.h>

#define HEADBIN_MAX (16 * 1024)

// True if head is a header of this layout whose three digests are right, so
// one made from it will be right too.
bool headbin_sound(const uint8_t *head, uint32_t len);
// The nine characters of its title ID and a NUL.
void headbin_title(const uint8_t *head, char titleid[10]);
// Puts another title ID in and makes the digests fit. False, with head
// unchanged, if head isn't sound or titleid isn't nine characters.
bool headbin_retitle(uint8_t *head, uint32_t len, const char *titleid);

#endif
