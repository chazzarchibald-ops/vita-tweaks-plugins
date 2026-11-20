#include "files.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdlib.h>
#include <string.h>

bool file_exists(const char *path) {
	SceIoStat st;
	return sceIoGetstat(path, &st) >= 0;
}

// The whole file, NUL-terminated, or NULL. *len gets its size.
char *file_read(const char *path, int max, int *len) {
	SceIoStat st;
	if (sceIoGetstat(path, &st) < 0 || st.st_size < 0 || st.st_size > max)
		return NULL;
	int size = (int)st.st_size;
	char *buf = malloc(size + 1);
	if (!buf)
		return NULL;
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	int got = 0;
	if (fd >= 0) {
		while (got < size) {
			int n = sceIoRead(fd, buf + got, size - got);
			if (n <= 0)
				break;
			got += n;
		}
		sceIoClose(fd);
	}
	if (fd < 0 || got != size) {
		free(buf);
		return NULL;
	}
	buf[size] = '\0';
	*len = size;
	return buf;
}

static int write_error;

int file_write_error(void) {
	return write_error;
}

bool file_write(const char *path, const char *buf, int len) {
	write_error = 0;
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) {
		write_error = fd;
		return false;
	}
	int done = 0;
	while (done < len) {
		int n = sceIoWrite(fd, buf + done, len - done);
		if (n <= 0) {
			write_error = n;
			break;
		}
		done += n;
	}
	sceIoClose(fd);
	return done == len;
}

// Written, then read back and compared.
bool file_write_checked(const char *path, const char *buf, int len) {
	if (!file_write(path, buf, len))
		return false;
	int back_len = 0;
	char *back = file_read(path, len + 1, &back_len);
	bool same = back && back_len == len && memcmp(back, buf, len) == 0;
	free(back);
	return same;
}

bool file_copy(const char *from, const char *to, int max) {
	int len = 0;
	char *buf = file_read(from, max, &len);
	bool ok = buf && file_write_checked(to, buf, len);
	free(buf);
	return ok;
}

void file_remove_tree(const char *path) {
	SceUID dir = sceIoDopen(path);
	if (dir < 0) {
		sceIoRemove(path); // a file, or nothing
		return;
	}
	SceIoDirent entry;
	for (;;) {
		memset(&entry, 0, sizeof(entry));
		if (sceIoDread(dir, &entry) <= 0)
			break;
		char child[256];
		if (strlen(path) + 1 + strlen(entry.d_name) >= sizeof(child))
			continue;
		strcpy(child, path);
		strcat(child, "/");
		strcat(child, entry.d_name);
		if (SCE_S_ISDIR(entry.d_stat.st_mode))
			file_remove_tree(child);
		else
			sceIoRemove(child);
	}
	sceIoDclose(dir);
	sceIoRmdir(path);
}
