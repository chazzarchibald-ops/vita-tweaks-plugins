#include "projects.h"

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "files.h"
#include "headbin.h"
#include "json.h"
#include "net.h"
#include "package.h"
#include "sha256.h"
#include "unzip.h"

#define API "https://api.github.com/repos/"
#define ANSWER_MAX (256 * 1024)
#define OWN_HEAD "app0:sce_sys/package/head.bin" // made by whatever installed this app
#define WORK_DIR "ux0:data/tweaks_projects"
#define APP_DIR WORK_DIR "/app"          // an app, laid out for the system's installer
#define ZIP_PATH WORK_DIR "/download.zip" // a zip of data files, while it is unpacked
#define ENTRIES_MAX 2048
#define MB (1024 * 1024)

static ZipEntry entries[ENTRIES_MAX];

// ---- small things ----

// True for a file name from a zip that stays inside the folder it is unpacked into.
static bool safe_name(const ZipEntry *e) {
	if (e->name_len <= 0 || e->name_len > 180 || e->name[0] == '/')
		return false;
	for (int i = 0; i < e->name_len; i++) {
		char c = e->name[i];
		if (c == '\\' || c == ':' || c == '\0' || (c == '.' && i + 1 < e->name_len && e->name[i + 1] == '.'))
			return false;
	}
	return true;
}

static bool is_folder(const ZipEntry *e) {
	return e->name[e->name_len - 1] == '/';
}

static bool is_name(const ZipEntry *e, const char *name) {
	return e->name_len == (int)strlen(name) && memcmp(e->name, name, (size_t)e->name_len) == 0;
}

// Makes the folders a path's file sits in.
static void make_folders(const char *path) {
	char dir[256];
	snprintf(dir, sizeof(dir), "%s", path);
	for (char *p = strchr(dir, ':'); p && (p = strchr(p + 1, '/')) != NULL;) {
		*p = '\0';
		sceIoMkdir(dir, 0777);
		*p = '/';
	}
}

static uint32_t u16(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t u32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A string of a param.sfo, by its key. False if it isn't there or doesn't fit.
static bool sfo_string(const uint8_t *sfo, uint32_t len, const char *key, char *out, int size) {
	if (len < 20 || memcmp(sfo, "\0PSF", 4) != 0)
		return false;
	uint32_t keys = u32(sfo + 8), data = u32(sfo + 12), count = u32(sfo + 16);
	for (uint32_t i = 0; i < count && 20 + (i + 1) * 16 <= len; i++) {
		const uint8_t *e = sfo + 20 + i * 16;
		uint32_t key_at = keys + u16(e), value_len = u32(e + 4), value_at = data + u32(e + 12);
		if (key_at >= len || strlen(key) >= len - key_at || strcmp((const char *)sfo + key_at, key) != 0)
			continue;
		if (u16(e + 2) != 0x0204 || value_at > len || value_len > len - value_at)
			return false; // not text, or not inside the file
		uint32_t n = 0;
		while (n < value_len && sfo[value_at + n])
			n++;
		if ((int)n >= size)
			return false;
		memcpy(out, sfo + value_at, n);
		out[n] = '\0';
		return true;
	}
	return false;
}

// ---- progress, on the line that is already on screen ----

static UpdateSay progress_say;
static const char *progress_name;
static int progress_shown;

static void progress_begin(UpdateSay say, const char *name) {
	progress_say = say;
	progress_name = name;
	progress_shown = -1;
}

// For a download of a few megabytes: tenths.
static void small_progress(int got, int total) {
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
	if (total <= 0)
		return;
	int tenth = got / 1024 * 10 / (total / 1024 + 1);
	if (tenth != progress_shown) {
		progress_shown = tenth;
		progress_say(SAY_DIM, true, "Downloading %s... %d%%", progress_name, tenth * 10);
	}
}

// For a download of hundreds: megabytes, every 4 of them.
static bool big_progress(int got, int total) {
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); // the Vita stays awake
	int step = got / (4 * MB);
	if (step != progress_shown) {
		progress_shown = step;
		if (total > 0)
			progress_say(SAY_DIM, true, "Downloading %s... %d of %d MB", progress_name, got / MB, total / MB);
		else
			progress_say(SAY_DIM, true, "Downloading %s... %d MB", progress_name, got / MB);
	}
	return true;
}

// ---- what GitHub lists ----

bool project_find(const char *repo, const char *tag, const char *name, ProjectFile *file, UpdateSay say) {
	char why[160], url[320];
	memset(file, 0, sizeof(*file));
	say(SAY_DIM, false, "Asking GitHub for the release...");
	if (!net_start(why, sizeof(why))) {
		say(SAY_BAD, true, "%s", why);
		return false;
	}
	int n = snprintf(url, sizeof(url), API "%s/releases/%s", repo, tag && tag[0] ? "tags/" : "latest");
	// the tag as part of an address: anything but letters, digits and . _ - as %XX
	for (const unsigned char *c = (const unsigned char *)(tag ? tag : ""); *c && n < (int)sizeof(url) - 4; c++) {
		bool plain = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '.' ||
		             *c == '_' || *c == '-';
		n += snprintf(url + n, sizeof(url) - (size_t)n, plain ? "%c" : "%%%02X", *c);
	}
	int len = 0;
	char *answer = net_get(url, "application/vnd.github+json", ANSWER_MAX, &len, NULL, why, sizeof(why));
	if (!answer) {
		say(SAY_BAD, true, "%s", why);
		return false;
	}
	bool ok = lookup_asset(answer, repo, name, file);
	if (!ok)
		say(SAY_BAD, true, "That release's %s isn't there, or isn't as expected.", name[0] == '*' ? name + 1 : name);
	free(answer);
	return ok;
}

uint8_t *project_fetch(const ProjectFile *file, int max, UpdateSay say) {
	char why[160], digest[65];
	if (file->size > max) {
		say(SAY_BAD, true, "%s is bigger than this app takes at once (%d MB).", file->name, file->size / MB);
		return NULL;
	}
	progress_begin(say, file->name);
	say(SAY_DIM, true, "Downloading %s... 0%%", file->name);
	int len = 0;
	uint8_t *data = (uint8_t *)net_get(file->url, NULL, max, &len, small_progress, why, sizeof(why));
	if (!data) {
		say(SAY_BAD, true, "%s", why);
		return NULL;
	}
	say(SAY_TEXT, true, "Downloaded %s (%d KB).", file->name, (len + 1023) / 1024);
	sha256_hex(data, (uint32_t)len, digest);
	if (len != file->size || strcmp(digest, file->digest) != 0) {
		free(data);
		say(SAY_BAD, false, "It isn't the file GitHub lists (its SHA-256 differs). Nothing was changed.");
		return NULL;
	}
	say(SAY_GOOD, false, "Its SHA-256 is the one GitHub lists.");
	return data;
}

// ---- an app ----

bool project_install_vpk(const uint8_t *vpk, int len, char *title, int title_size, UpdateSay say) {
	char path[256], id[16] = "", name[64] = "", own[10] = "";
	int error = 0;
	int count = zip_entries(vpk, (uint32_t)len, entries, ENTRIES_MAX);
	if (count <= 0 || count >= ENTRIES_MAX) {
		say(SAY_BAD, false, "The download isn't a VPK this app can unpack. Nothing was changed.");
		return false;
	}
	// what it is, from its param.sfo
	bool has_eboot = false;
	for (int i = 0; i < count; i++) {
		has_eboot |= is_name(&entries[i], "eboot.bin");
		if (!is_name(&entries[i], "sce_sys/param.sfo"))
			continue;
		uint8_t *sfo = zip_extract(vpk, (uint32_t)len, &entries[i]);
		if (sfo) {
			sfo_string(sfo, entries[i].size, "TITLE_ID", id, sizeof(id));
			sfo_string(sfo, entries[i].size, "TITLE", name, sizeof(name));
		}
		free(sfo);
	}
	bool id_ok = strlen(id) == 9;
	for (int i = 0; id_ok && i < 9; i++)
		id_ok = (id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9');
	if (!has_eboot || !id_ok || strcmp(id, "TWKS00001") == 0 || strcmp(id, "TWKS00002") == 0) {
		say(SAY_BAD, false, "The download isn't an app this can install. Nothing was changed.");
		return false;
	}
	if (title)
		snprintf(title, (size_t)title_size, "%s", name[0] ? name : id);

	// the package header: this app's own, with the other app's title ID put in
	int head_len = 0;
	uint8_t *head = (uint8_t *)file_read(OWN_HEAD, HEADBIN_MAX, &head_len);
	if (head && headbin_sound(head, (uint32_t)head_len))
		headbin_title(head, own);
	if (strcmp(own, "TWKS00001") != 0 || !headbin_retitle(head, (uint32_t)head_len, id)) {
		free(head);
		say(SAY_BAD, false, "This app's package header (%s) isn't one it can install with.", OWN_HEAD);
		return false;
	}

	// its files, as the system's installer wants them
	file_remove_tree(WORK_DIR);
	say(SAY_DIM, false, "Unpacking %s...", name[0] ? name : id);
	bool written = true;
	int files = 0;
	for (int i = 0; written && i < count; i++) {
		if (is_folder(&entries[i]))
			continue;
		written = safe_name(&entries[i]);
		uint8_t *data = written ? zip_extract(vpk, (uint32_t)len, &entries[i]) : NULL;
		if (!data) {
			written = false;
			break;
		}
		snprintf(path, sizeof(path), APP_DIR "/%.*s", entries[i].name_len, entries[i].name);
		make_folders(path);
		written = file_write(path, (const char *)data, (int)entries[i].size);
		free(data);
		if (written && ++files % 40 == 0) {
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
			say(SAY_DIM, true, "Unpacking %s... %d of %d files", name[0] ? name : id, files, count);
		}
	}
	make_folders(APP_DIR "/sce_sys/package/head.bin");
	written = written && file_write_checked(APP_DIR "/sce_sys/package/head.bin", (const char *)head, head_len);
	free(head);
	if (!written) {
		file_remove_tree(WORK_DIR);
		say(SAY_BAD, true, "Couldn't unpack the VPK into " APP_DIR ". Nothing was changed.");
		return false;
	}
	say(SAY_TEXT, true, "Unpacked %d files. Installing...", files);

	if (!package_start(&error)) {
		file_remove_tree(WORK_DIR);
		say(SAY_BAD, false, "Couldn't start the system's installer (0x%08X). Nothing was changed.", (unsigned)error);
		return false;
	}
	error = package_install(APP_DIR);
	file_remove_tree(WORK_DIR);
	if (error < 0) {
		say(SAY_BAD, false, "The system wouldn't install it (0x%08X).", (unsigned)error);
		return false;
	}
	say(SAY_GOOD, true, "Installed %s. Its bubble is on the home screen.", name[0] ? name : id);
	return true;
}

// ---- a zip of data files ----

static int read_fd(void *from, uint32_t offset, uint8_t *buf, uint32_t len) {
	SceUID fd = *(SceUID *)from;
	if (sceIoLseek(fd, (SceOff)offset, SCE_SEEK_SET) < 0)
		return -1;
	uint32_t got = 0;
	while (got < len) {
		int n = sceIoRead(fd, buf + got, len - got);
		if (n <= 0)
			break;
		got += (uint32_t)n;
	}
	return (int)got;
}

// Where a file being unpacked goes, and how far the whole unpacking has got.
typedef struct {
	SceUID fd;
	UpdateSay say;
	uint32_t done, total; // in KB, so hundreds of megabytes fit
	int shown;
} Unpack;

static bool write_fd(void *to, const uint8_t *buf, uint32_t len) {
	Unpack *u = to;
	for (uint32_t done = 0; done < len;) {
		int n = sceIoWrite(u->fd, buf + done, len - done);
		if (n <= 0)
			return false;
		done += (uint32_t)n;
	}
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
	u->done += (len + 512) / 1024;
	int step = (int)(u->done / (8 * 1024));
	if (step != u->shown) {
		u->shown = step;
		u->say(SAY_DIM, true, "Unpacking... %u of %u MB", u->done / 1024, u->total / 1024);
	}
	return true;
}

bool project_install_zip(const ProjectFile *file, const char *folder, const char *into, UpdateSay say) {
	char why[160], digest[65], path[256];
	uint64_t max_size = 0, free_size = 0;
	// room for the zip and for what comes out of it, which is about as big
	uint64_t need = (uint64_t)file->size * 2 + 32 * MB;
	if (sceAppMgrGetDevInfo("ux0:", &max_size, &free_size) >= 0 && free_size < need) {
		say(SAY_BAD, false, "%s needs about %u MB free on the memory card while it unpacks. There are %u.", file->name,
		    (unsigned)(need / MB), (unsigned)(free_size / MB));
		return false;
	}
	file_remove_tree(WORK_DIR);
	make_folders(ZIP_PATH);
	progress_begin(say, file->name);
	say(SAY_DIM, false, "Downloading %s... 0 of %d MB", file->name, file->size / MB);
	int len = 0;
	if (!net_get_file(file->url, ZIP_PATH, (unsigned)file->size, &len, digest, big_progress, why, sizeof(why))) {
		file_remove_tree(WORK_DIR);
		say(SAY_BAD, false, "%s", why);
		return false;
	}
	say(SAY_TEXT, true, "Downloaded %s (%d MB).", file->name, len / MB);
	if (len != file->size || strcmp(digest, file->digest) != 0) {
		file_remove_tree(WORK_DIR);
		say(SAY_BAD, false, "It isn't the file GitHub lists (its SHA-256 differs). Nothing was unpacked.");
		return false;
	}
	say(SAY_GOOD, false, "Its SHA-256 is the one GitHub lists.");

	SceUID zip = sceIoOpen(ZIP_PATH, SCE_O_RDONLY, 0);
	uint8_t *names = NULL;
	int count = zip >= 0 ? zip_file_entries(read_fd, &zip, (uint32_t)len, entries, ENTRIES_MAX, &names) : -1;
	// every file is where it is expected: under folder/, and nowhere else
	size_t folder_len = strlen(folder);
	bool sound = count > 0 && count < ENTRIES_MAX;
	Unpack unpack = {-1, say, 0, 0, -1};
	for (int i = 0; sound && i < count; i++) {
		sound = safe_name(&entries[i]) && entries[i].name_len > (int)folder_len &&
		        memcmp(entries[i].name, folder, folder_len) == 0 && entries[i].name[folder_len] == '/';
		unpack.total += (entries[i].size + 512) / 1024;
	}
	if (!sound) {
		say(SAY_BAD, false, "%s isn't laid out as expected (everything under %s/). Nothing was unpacked.", file->name,
		    folder);
		goto fail;
	}
	say(SAY_DIM, false, "Unpacking... 0 of %u MB", unpack.total / 1024);
	int files = 0;
	for (int i = 0; i < count; i++) {
		snprintf(path, sizeof(path), "%s/%.*s", into, entries[i].name_len, entries[i].name);
		if (is_folder(&entries[i])) {
			make_folders(path); // (the path ends in a slash, so this makes the folder itself)
			continue;
		}
		make_folders(path);
		unpack.fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
		bool ok = unpack.fd >= 0 && zip_file_extract(read_fd, &zip, (uint32_t)len, &entries[i], write_fd, &unpack);
		if (unpack.fd >= 0)
			sceIoClose(unpack.fd);
		if (!ok) {
			sceIoRemove(path);
			say(SAY_BAD, false, "Couldn't unpack %.*s. Is the memory card full?", entries[i].name_len, entries[i].name);
			say(SAY_TEXT, false, "%d of the files are in %s/%s; the rest are missing.", files, into, folder);
			goto fail;
		}
		files++;
	}
	free(names);
	sceIoClose(zip);
	file_remove_tree(WORK_DIR);
	say(SAY_GOOD, true, "Unpacked %d files to %s/%s.", files, into, folder);
	return true;
fail:
	free(names);
	if (zip >= 0)
		sceIoClose(zip);
	file_remove_tree(WORK_DIR);
	return false;
}
