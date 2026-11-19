#include "update.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
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

#define REPO_API "https://api.github.com/repos/chazzarchibald-ops/vita-tweaks-plugins"
#ifdef UPDATE_TEST_TAG
// A test build (see CMakeLists.txt): one release by its tag, which finds a
// pre-release too. "The latest" never is one.
#define RELEASES REPO_API "/releases/tags/" UPDATE_TEST_TAG
#define ASKING "Asking GitHub for the " UPDATE_TEST_TAG " release (update test)..."
#else
#define RELEASES REPO_API "/releases/latest"
#define ASKING "Asking GitHub for the latest release..."
#endif
#define DOWNLOADS "https://github.com/chazzarchibald-ops/vita-tweaks-plugins/releases/download/"
#define ASSET "tweaks_installer.vpk"
#define TITLE_ID "TWKS00001"
#define KEPT_VPK "ux0:data/" ASSET
// The updater: this program again, installed under another title ID (see "installing it").
#define HELPER_ID "TWKS00002"
#define HELPER_MARK "updater.txt"                // in the updater's folder, and not in the installer's
#define HELPER_SFO "app0:updater/param.sfo"      // the updater's param.sfo, which the VPK carries
#define OWN_HEAD "app0:sce_sys/package/head.bin" // made by whatever installed this app
#define WORK_DIR "ux0:data/tweaks_update"
#define HELPER_DIR WORK_DIR "/helper" // the updater, laid out to be installed
#define PKG_DIR WORK_DIR "/pkg"       // the new installer, likewise
#define JOB_FILE WORK_DIR "/job.txt"  // what the updater is to install
#define HEAD_FILE WORK_DIR "/head.bin" // the header to install it with
#define EBOOT_MAX (8 * 1024 * 1024)
#define ANSWER_MAX (256 * 1024)
#define VPK_MAX (16 * 1024 * 1024)
#define FILES_MAX 64

// ---- GitHub's answer ----

// "1.10" against "1.9": more than 0 if a is the newer. A leading v is ignored.
static int version_compare(const char *a, const char *b) {
	if (*a == 'v' || *a == 'V')
		a++;
	if (*b == 'v' || *b == 'V')
		b++;
	for (;;) {
		int x = 0, y = 0;
		for (; *a >= '0' && *a <= '9'; a++)
			x = x * 10 + (*a - '0');
		for (; *b >= '0' && *b <= '9'; b++)
			y = y * 10 + (*b - '0');
		if (x != y)
			return x - y;
		if (*a != '.' && *b != '.')
			return 0;
		if (*a == '.')
			a++;
		if (*b == '.')
			b++;
	}
}

static bool is_version(const char *s) {
	if (*s == 'v' || *s == 'V')
		s++;
	if (*s < '0' || *s > '9')
		return false;
	for (; *s; s++)
		if ((*s < '0' || *s > '9') && *s != '.')
			return false;
	return true;
}

UpdateFound update_check(const char *have, UpdateInfo *info, UpdateSay say) {
	char why[160];
	memset(info, 0, sizeof(*info));
	say(SAY_DIM, false, ASKING);
	if (!net_start(why, sizeof(why))) {
		say(SAY_BAD, false, "%s", why);
		return UPDATE_FAILED;
	}
	int len = 0;
	char *answer = net_get(RELEASES, "application/vnd.github+json", ANSWER_MAX, &len, NULL, why, sizeof(why));
	if (!answer) {
		say(SAY_BAD, false, "%s", why);
		return UPDATE_FAILED;
	}
	UpdateFound found = UPDATE_FAILED;
	if (!json_string(answer, "tag_name", info->tag, sizeof(info->tag)) || !is_version(info->tag)) {
		say(SAY_BAD, false, "GitHub's answer has no version in it.");
	} else if (version_compare(info->tag, have) <= 0) {
		say(SAY_GOOD, false, "This installer has the latest Tweaks/Plugins (%s).", have);
		found = UPDATE_NONE;
	} else {
		// the release's file named ASSET, and what GitHub says of it
		const char *asset = NULL;
		char name[64];
		for (const char *at = answer; (at = strstr(at, "\"name\"")) != NULL; at++)
			if (json_string(at, "name", name, sizeof(name)) && strcmp(name, ASSET) == 0) {
				asset = at;
				break;
			}
		char digest[80] = "";
		if (asset) {
			info->size = json_number(asset, "size");
			json_string(asset, "digest", digest, sizeof(digest));
			json_string(asset, "browser_download_url", info->url, sizeof(info->url));
		}
		say(SAY_TEXT, false, "Tweaks/Plugins %s is out. This installer has %s.", info->tag, have);
		if (!asset) {
			say(SAY_BAD, false, "That release has no " ASSET " to update with.");
		} else if (strncmp(info->url, DOWNLOADS, strlen(DOWNLOADS)) != 0 || info->size <= 0 || info->size > VPK_MAX ||
		           strncmp(digest, "sha256:", 7) != 0 || strlen(digest + 7) != 64) {
			say(SAY_BAD, false, "GitHub's details of " ASSET " aren't as expected.");
		} else {
			memcpy(info->digest, digest + 7, 65);
			say(SAY_GOOD, false, "It can be downloaded (%d KB) and put in place of this one.", (info->size + 1023) / 1024);
			found = UPDATE_READY;
		}
	}
	free(answer);
	return found;
}

// ---- installing it ----
//
// An app can't write to its own folder in ux0:app, and the system keeps its
// own records of an installed app, so the new version has to go in through
// the system's installer (package.h), and not while this app is running. So,
// as other self-updating homebrew does:
//   1. this app downloads and checks the VPK and keeps it in ux0:data;
//   2. it installs an updater, which is this same program under another title
//      ID with a file beside it that says so, and opens it;
//   3. the updater unpacks the VPK, has the system install it over this app,
//      and opens the result, which deletes the updater.

static UpdateSay progress_say;
static int progress_shown;

static void download_progress(int got, int total) {
	if (total <= 0)
		return;
	int tenth = got / 1024 * 10 / (total / 1024 + 1); // (in KB, so a big file's count fits)
	if (tenth != progress_shown) {
		progress_shown = tenth;
		progress_say(SAY_DIM, true, "Downloading " ASSET "... %d%%", tenth * 10);
	}
}

// A file name of the VPK as a path under dir. False for a name that could
// lead out of dir, or that doesn't fit.
static bool path_in(const char *dir, const ZipEntry *e, char *path, int size) {
	if (e->name_len <= 0 || e->name[0] == '/' || (int)strlen(dir) + 1 + e->name_len >= size)
		return false;
	for (int i = 0; i < e->name_len; i++) {
		char c = e->name[i];
		if (c == '\\' || c == ':' || c == '\0' || (c == '.' && i + 1 < e->name_len && e->name[i + 1] == '.'))
			return false;
	}
	snprintf(path, (size_t)size, "%s/%.*s", dir, e->name_len, e->name);
	return true;
}

static bool is_name(const ZipEntry *e, const char *name) {
	return e->name_len == (int)strlen(name) && memcmp(e->name, name, (size_t)e->name_len) == 0;
}

static bool contains(const uint8_t *data, uint32_t len, const char *word) {
	uint32_t n = (uint32_t)strlen(word);
	for (uint32_t i = 0; i + n <= len; i++)
		if (memcmp(data + i, word, n) == 0)
			return true;
	return false;
}

// Makes the folders a path's file sits in.
static void make_folders(const char *path) {
	char dir[200];
	snprintf(dir, sizeof(dir), "%s", path);
	for (char *p = strchr(dir, ':'); p && (p = strchr(p + 1, '/')) != NULL;) {
		*p = '\0';
		sceIoMkdir(dir, 0777);
		*p = '/';
	}
}

// The VPK's files, each unpacked whole into files[] (NULL for a folder).
// False, with nothing left in files[], unless it unpacks whole, every name
// stays inside the app's folder, and it is this app: an eboot.bin, the
// plugin, and a param.sfo with this app's title ID.
static bool vpk_unpack(const uint8_t *vpk, int len, ZipEntry *entries, int *count, uint8_t **files) {
	char path[200];
	memset(files, 0, sizeof(files[0]) * FILES_MAX);
	*count = zip_entries(vpk, (uint32_t)len, entries, FILES_MAX);
	bool has_eboot = false, has_plugin = false, is_this_app = false, sound = *count > 0 && *count < FILES_MAX;
	for (int i = 0; sound && i < *count; i++) {
		if (entries[i].name_len > 0 && entries[i].name[entries[i].name_len - 1] == '/')
			continue; // a folder
		sound = path_in(PKG_DIR, &entries[i], path, sizeof(path)) &&
		        (files[i] = zip_extract(vpk, (uint32_t)len, &entries[i])) != NULL;
		if (!sound)
			break;
		has_eboot |= is_name(&entries[i], "eboot.bin");
		has_plugin |= is_name(&entries[i], "tweaks.suprx");
		if (is_name(&entries[i], "sce_sys/param.sfo"))
			is_this_app = contains(files[i], entries[i].size, TITLE_ID);
	}
	if (sound && has_eboot && has_plugin && is_this_app)
		return true;
	for (int i = 0; i < FILES_MAX; i++) {
		free(files[i]);
		files[i] = NULL;
	}
	return false;
}

// The value of "key=" in the job file's text, into out. False if it isn't there or doesn't fit.
static bool job_value(const char *job, const char *key, char *out, int size) {
	char find[24];
	snprintf(find, sizeof(find), "%s=", key);
	const char *at = strstr(job, find);
	if (!at || (at != job && at[-1] != '\n'))
		return false;
	at += strlen(find);
	int n = 0;
	while (at[n] && at[n] != '\n' && at[n] != '\r')
		n++;
	if (n >= size)
		return false;
	memcpy(out, at, (size_t)n);
	out[n] = '\0';
	return true;
}

// Step 2: the updater, laid out in HELPER_DIR and installed.
static bool updater_install(const char *digest, int size, const char *tag, UpdateSay say) {
	int head_len = 0, error = 0;
	uint8_t *head = (uint8_t *)file_read(OWN_HEAD, HEADBIN_MAX, &head_len);
	char title[10] = "";
	if (head && headbin_sound(head, (uint32_t)head_len))
		headbin_title(head, title);
	if (strcmp(title, TITLE_ID) != 0) {
		free(head);
		say(SAY_BAD, false, "This app's package header (%s) isn't one it can install with.", OWN_HEAD);
		return false;
	}
	file_remove_tree(WORK_DIR);
	make_folders(HELPER_DIR "/sce_sys/package/head.bin");
	make_folders(HELPER_DIR "/font/x");
	char job[160];
	snprintf(job, sizeof(job), "sha256=%s\nsize=%d\ntag=%s\n", digest, size, tag);
	// for the updater: what it is to install, and the header to install it with
	bool ok = file_write_checked(JOB_FILE, job, (int)strlen(job)) &&
	          file_write_checked(HEAD_FILE, (const char *)head, head_len);
	// the updater itself
	ok = ok && headbin_retitle(head, (uint32_t)head_len, HELPER_ID) &&
	     file_write_checked(HELPER_DIR "/sce_sys/package/head.bin", (const char *)head, head_len) &&
	     file_copy("app0:eboot.bin", HELPER_DIR "/eboot.bin", EBOOT_MAX) &&
	     file_copy(HELPER_SFO, HELPER_DIR "/sce_sys/param.sfo", 64 * 1024) &&
	     file_write_checked(HELPER_DIR "/" HELPER_MARK, "1\n", 2);
	free(head);
	if (!ok) {
		say(SAY_BAD, false, "Couldn't set the updater up in " WORK_DIR ".");
		return false;
	}
	// (how it looks, which it can do without)
	file_copy("app0:sce_sys/icon0.png", HELPER_DIR "/sce_sys/icon0.png", 1024 * 1024);
	file_copy("app0:font/Poppins-Regular.ttf", HELPER_DIR "/font/Poppins-Regular.ttf", 2 * 1024 * 1024);
	file_copy("app0:font/Righteous-Regular.ttf", HELPER_DIR "/font/Righteous-Regular.ttf", 2 * 1024 * 1024);

	if (!package_start(&error)) {
		say(SAY_BAD, false, "Couldn't start the system's installer (0x%08X).", (unsigned)error);
		return false;
	}
	if (package_installed(HELPER_ID))
		package_remove(HELPER_ID);
	error = package_install(HELPER_DIR);
	if (error < 0) {
		say(SAY_BAD, false, "The system wouldn't install the updater (0x%08X).", (unsigned)error);
		return false;
	}
	return true;
}

bool update_install(const UpdateInfo *info, UpdateSay say) {
	static ZipEntry entries[FILES_MAX];
	uint8_t *files[FILES_MAX];
	char why[160];
	bool ok = false;
	int count = 0;

	if (!net_start(why, sizeof(why))) {
		say(SAY_BAD, false, "%s", why);
		return false;
	}
	progress_say = say;
	progress_shown = 0;
	say(SAY_DIM, false, "Downloading " ASSET "... 0%%");
	int len = 0;
	uint8_t *vpk = (uint8_t *)net_get(info->url, NULL, VPK_MAX, &len, download_progress, why, sizeof(why));
	if (!vpk) {
		say(SAY_BAD, false, "%s", why);
		return false;
	}
	say(SAY_TEXT, true, "Downloaded " ASSET " (%d bytes).", len);

	// it is the file GitHub's release lists
	char digest[65];
	sha256_hex(vpk, (uint32_t)len, digest);
	if (len != info->size || strcmp(digest, info->digest) != 0) {
		say(SAY_BAD, false, "The download isn't the file GitHub lists (its SHA-256 differs). Nothing was changed.");
		goto done;
	}
	say(SAY_GOOD, false, "Its SHA-256 is the one GitHub lists.");

	// it unpacks whole, and it is this app
	if (!vpk_unpack(vpk, len, entries, &count, files)) {
		say(SAY_BAD, false, "The download doesn't unpack as the Tweaks/Plugins Installer. Nothing was changed.");
		goto done;
	}
	for (int i = 0; i < FILES_MAX; i++)
		free(files[i]);

	// where the updater will find it, and where it stays to install by hand
	if (!file_write_checked(KEPT_VPK, (const char *)vpk, len)) {
		say(SAY_BAD, false, "Couldn't keep the VPK at " KEPT_VPK ". Nothing was changed.");
		goto done;
	}
	say(SAY_DIM, false, "Kept the VPK at " KEPT_VPK ".");

	ok = updater_install(digest, len, info->tag, say);
	if (ok)
		say(SAY_GOOD, false, "Tweaks/Plugins %s is ready to install. The updater does it, and opens the new installer.",
		    info->tag);
	else
		say(SAY_TEXT, false, "Nothing was changed. You can install the kept VPK with VitaShell instead.");
done:
	free(vpk);
	return ok;
}

void update_open_updater(void) {
	package_launch(HELPER_ID);
}

bool update_is_updater(void) {
	return file_exists("app0:" HELPER_MARK);
}

// Step 3, in the updater.
bool update_finish(UpdateSay say) {
	static ZipEntry entries[FILES_MAX];
	uint8_t *files[FILES_MAX];
	char path[200], want[65] = "", size_text[16] = "", tag[24] = "", digest[65];
	uint8_t *vpk = NULL, *head = NULL;
	bool ok = false;
	int count = 0, len = 0, job_len = 0, head_len = 0, error = 0;
	memset(files, 0, sizeof(files));

	char *job = file_read(JOB_FILE, 1024, &job_len);
	if (!job || !job_value(job, "sha256", want, sizeof(want)) || !job_value(job, "size", size_text, sizeof(size_text)) ||
	    !job_value(job, "tag", tag, sizeof(tag))) {
		free(job);
		say(SAY_BAD, false, "There is no update waiting in " WORK_DIR ".");
		return false;
	}
	free(job);
	say(SAY_DIM, false, "Installing Tweaks/Plugins %s from " KEPT_VPK "...", tag);

	// the VPK is still the one the installer checked
	vpk = (uint8_t *)file_read(KEPT_VPK, VPK_MAX, &len);
	if (vpk)
		sha256_hex(vpk, (uint32_t)len, digest);
	if (!vpk || strcmp(digest, want) != 0) {
		say(SAY_BAD, false, "The VPK at " KEPT_VPK " isn't the one that was downloaded. Nothing was changed.");
		goto done;
	}
	if (!vpk_unpack(vpk, len, entries, &count, files)) {
		say(SAY_BAD, false, "The VPK doesn't unpack as the Tweaks/Plugins Installer. Nothing was changed.");
		goto done;
	}
	head = (uint8_t *)file_read(HEAD_FILE, HEADBIN_MAX, &head_len);
	char title[10] = "";
	if (head && headbin_sound(head, (uint32_t)head_len))
		headbin_title(head, title);
	if (strcmp(title, TITLE_ID) != 0) {
		say(SAY_BAD, false, "The package header in " WORK_DIR " isn't the installer's. Nothing was changed.");
		goto done;
	}

	// its files as the system's installer wants them
	file_remove_tree(PKG_DIR);
	bool written = true;
	for (int i = 0; written && i < count; i++) {
		if (!files[i])
			continue;
		path_in(PKG_DIR, &entries[i], path, sizeof(path));
		make_folders(path);
		written = file_write_checked(path, (const char *)files[i], (int)entries[i].size);
	}
	make_folders(PKG_DIR "/sce_sys/package/head.bin");
	written = written && file_write_checked(PKG_DIR "/sce_sys/package/head.bin", (const char *)head, head_len);
	if (!written) {
		file_remove_tree(PKG_DIR);
		say(SAY_BAD, false, "Couldn't unpack the VPK into " PKG_DIR ". Nothing was changed.");
		goto done;
	}
	say(SAY_TEXT, false, "Unpacked %d files. Installing...", count);

	if (!package_start(&error)) {
		say(SAY_BAD, false, "Couldn't start the system's installer (0x%08X). Nothing was changed.", (unsigned)error);
		goto done;
	}
	error = package_install(PKG_DIR);
	if (error < 0) {
		say(SAY_BAD, false, "The system wouldn't install it (0x%08X).", (unsigned)error);
		goto done;
	}
	ok = true;
	file_remove_tree(WORK_DIR);
	say(SAY_GOOD, false, "The installer is updated, with Tweaks/Plugins %s. Opening it...", tag);
done:
	if (!ok)
		say(SAY_TEXT, false, "You can install " KEPT_VPK " with VitaShell instead.");
	for (int i = 0; i < FILES_MAX; i++)
		free(files[i]);
	free(head);
	free(vpk);
	return ok;
}

void update_open_installer(void) {
	package_launch(TITLE_ID);
}

void update_tidy(void) {
	if (!file_exists("ux0:app/" HELPER_ID "/eboot.bin"))
		return;
	int error = 0;
	if (package_start(&error))
		package_remove(HELPER_ID);
	file_remove_tree(WORK_DIR);
}
