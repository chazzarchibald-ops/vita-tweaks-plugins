// Whole files at a time.
#ifndef FILES_H
#define FILES_H

#include <stdbool.h>

bool file_exists(const char *path);
// The whole file, NUL-terminated and malloc'd, or NULL (also when it is over max). *len gets its size.
char *file_read(const char *path, int max, int *len);
bool file_write(const char *path, const char *buf, int len);
// The system's answer when the last file_write failed (0 if it only wrote short).
int file_write_error(void);
// Written, then read back and compared.
bool file_write_checked(const char *path, const char *buf, int len);
// A copy of a file of up to max bytes, read back and compared.
bool file_copy(const char *from, const char *to, int max);
// Deletes a folder and all that is in it, or a file. Nothing happens if it isn't there.
void file_remove_tree(const char *path);

#endif
