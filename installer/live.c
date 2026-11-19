#include "live.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "files.h"
#include "lookup.h"
#include "net.h"
#include "png.h"

#define API "https://api.github.com/repos/"
#define CACHE_DIR "ux0:data/tweaks_installer"
#define PHOTO_DIR CACHE_DIR "/photos"
#define RELEASE_DIR CACHE_DIR "/releases"
#define ANSWER_MAX (512 * 1024)
#define SHOT_MAX (6 * 1024 * 1024)   // a screenshot's file
#define SHOT_PIXELS (4 * 1024 * 1024) // and its size unpacked
#define PHOTO_BYTES (PHOTO_W * PHOTO_H * 3)
#define PHOTO_FILL 0x2C1C18 // what shows beside a photo of another shape: the panel's dark blue
#define PROJECTS_MAX 4

typedef struct {
	LiveInfo info;
	Release release[LIVE_RELEASES]; // newest first
	char sha[LIVE_PHOTOS][41];      // the photos to show, in order: each is <sha>.rgb in its folder
	bool ask;
} Live;

static const LiveSource *sources;
static int source_count;
static Live live[PROJECTS_MAX];
static int generation;
static SceUID mutex = -1, wake = -1;

static void lock(void) {
	sceKernelLockMutex(mutex, 1, NULL);
}

static void unlock(void) {
	sceKernelUnlockMutex(mutex, 1);
}

static void photo_path(int project, const char *sha, char *path, int size) {
	snprintf(path, (size_t)size, PHOTO_DIR "/%s/%s.rgb", sources[project].key, sha);
}

static bool is_sha(const char *s) {
	if (strlen(s) != 40)
		return false;
	for (int i = 0; i < 40; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	return true;
}

// ---- the releases ----

// A project's releases from GitHub's answer, put where the screen reads them.
// False if the answer isn't a list of releases.
static bool take_releases(int project, const char *answer) {
	const LiveSource *src = &sources[project];
	Release *found = malloc(sizeof(Release) * LIVE_RELEASES);
	if (!found)
		return false;
	int n = lookup_releases(answer, src->repo, src->asset, RELEASE_FILES, found, LIVE_RELEASES);
	if (n >= 0) {
		const Release *newest = n ? &found[0] : NULL;
		for (int i = n - 1; i >= 0; i--)
			if (!found[i].prerelease)
				newest = &found[i]; // the newest that isn't a pre-release, if there is one
		lock();
		memcpy(live[project].release, found, sizeof(Release) * (size_t)n);
		live[project].info.releases = n;
		live[project].info.release[0] = live[project].info.date[0] = '\0';
		if (newest) {
			memcpy(live[project].info.release, newest->name, sizeof(newest->name));
			memcpy(live[project].info.date, newest->date, sizeof(newest->date));
		}
		generation++;
		unlock();
	}
	free(found);
	return n >= 0;
}

static void releases_path(int project, char *path, int size) {
	snprintf(path, (size_t)size, RELEASE_DIR "/%s.json", sources[project].key);
}

// ---- the photos that are kept ----

// The list a project's folder holds: index.txt, a sha a line, each with its photo there.
static void read_index(int project) {
	char path[200], sha[41];
	snprintf(path, sizeof(path), PHOTO_DIR "/%s/index.txt", sources[project].key);
	int len = 0, n = 0;
	char *text = file_read(path, 4096, &len);
	for (const char *p = text; p && *p && n < LIVE_PHOTOS;) {
		int k = 0;
		while (p[k] && p[k] != '\n' && p[k] != '\r')
			k++;
		if (k == 40) {
			memcpy(sha, p, 40);
			sha[40] = '\0';
			photo_path(project, sha, path, sizeof(path));
			if (is_sha(sha) && file_exists(path))
				memcpy(live[project].sha[n++], sha, 41);
		}
		p += k;
		while (*p == '\n' || *p == '\r')
			p++;
	}
	free(text);
	live[project].info.photos = n;
}

static void write_index(int project, char sha[][41], int n) {
	char path[200], text[LIVE_PHOTOS * 42 + 1];
	int at = 0;
	for (int i = 0; i < n; i++)
		at += snprintf(text + at, sizeof(text) - (size_t)at, "%s\n", sha[i]);
	snprintf(path, sizeof(path), PHOTO_DIR "/%s/index.txt", sources[project].key);
	file_write(path, text, at);
}

// Deletes the photos of a project's folder that its list no longer has.
static void drop_others(int project, char sha[][41], int n) {
	char dir[160], path[480];
	snprintf(dir, sizeof(dir), PHOTO_DIR "/%s", sources[project].key);
	SceUID d = sceIoDopen(dir);
	if (d < 0)
		return;
	SceIoDirent entry;
	for (;;) {
		memset(&entry, 0, sizeof(entry));
		if (sceIoDread(d, &entry) <= 0)
			break;
		size_t len = strlen(entry.d_name);
		if (len < 5 || strcmp(entry.d_name + len - 4, ".rgb") != 0)
			continue;
		bool kept = false;
		for (int i = 0; i < n && !kept; i++)
			kept = len == 44 && memcmp(entry.d_name, sha[i], 40) == 0;
		if (!kept) {
			snprintf(path, sizeof(path), "%s/%s", dir, entry.d_name);
			sceIoRemove(path);
		}
	}
	sceIoDclose(d);
}

// ---- asking ----

// The list so far goes up at once, so each photo shows as soon as it is there.
static void publish_photos(int project, char sha[][41], int n, bool more) {
	lock();
	memcpy(live[project].sha, sha, (size_t)n * 41);
	live[project].info.photos = n;
	live[project].info.getting_photos = more;
	generation++;
	unlock();
}

// A screenshot from GitHub as a photo in the project's folder. False if it couldn't be had.
static bool fetch_photo(int project, const Shot *shot) {
	char why[160], path[200], fresh[208];
	int len = 0, w = 0, h = 0;
	if (shot->size > SHOT_MAX)
		return false;
	uint8_t *png = (uint8_t *)net_get(shot->url, NULL, SHOT_MAX, &len, NULL, why, sizeof(why));
	if (!png)
		return false;
	uint8_t *rgb = png_decode(png, (uint32_t)len, SHOT_PIXELS, &w, &h);
	free(png);
	if (!rgb)
		return false;
	uint8_t *photo = photo_fit(rgb, w, h, PHOTO_W, PHOTO_H, PHOTO_FILL);
	free(rgb);
	if (!photo)
		return false;
	// written beside its place and moved in, so a photo that is there is always whole
	photo_path(project, shot->sha, path, sizeof(path));
	snprintf(fresh, sizeof(fresh), "%s.new", path);
	bool ok = file_write_checked(fresh, (const char *)photo, PHOTO_BYTES) && sceIoRename(fresh, path) >= 0;
	if (!ok)
		sceIoRemove(fresh);
	free(photo);
	return ok;
}

static void ask_project(int project) {
	const LiveSource *src = &sources[project];
	char why[160], url[256], path[200];
	int len = 0;

	lock();
	live[project].info.state = LIVE_LOOKING;
	live[project].info.getting_photos = true;
	generation++;
	unlock();

	// its releases, with their notes and the size of each download
	bool found = false;
	char *answer = NULL;
	if (net_start(why, sizeof(why))) {
		snprintf(url, sizeof(url), API "%s/releases?per_page=%d", src->repo, LIVE_RELEASES);
		answer = net_get(url, "application/vnd.github+json", ANSWER_MAX, &len, NULL, why, sizeof(why));
	}
	if (answer && take_releases(project, answer)) {
		found = true;
		sceIoMkdir(CACHE_DIR, 0777);
		sceIoMkdir(RELEASE_DIR, 0777);
		releases_path(project, path, sizeof(path));
		file_write(path, answer, len); // kept for the next time, and for when there is no network
	}
	free(answer);
	lock();
	live[project].info.state = found ? LIVE_FOUND : LIVE_FAILED;
	generation++;
	unlock();

	// its screenshots: the ones already kept are shown as they are, the rest fetched
	static Shot shots[LIVE_PHOTOS];
	char sha[LIVE_PHOTOS][41];
	int count = -1, n = 0;
	if (found) { // (GitHub answered a moment ago, so it is worth asking)
		snprintf(url, sizeof(url), API "%s/contents/%s", src->repo, src->shots);
		answer = net_get(url, "application/vnd.github+json", ANSWER_MAX, &len, NULL, why, sizeof(why));
		if (answer)
			count = lookup_shots(answer, src->repo, shots, LIVE_PHOTOS);
		free(answer);
	}
	if (count < 0) { // no list to compare with: what is kept stays
		lock();
		live[project].info.getting_photos = false;
		generation++;
		unlock();
		return;
	}
	sceIoMkdir(CACHE_DIR, 0777);
	sceIoMkdir(PHOTO_DIR, 0777);
	snprintf(path, sizeof(path), PHOTO_DIR "/%s", src->key);
	sceIoMkdir(path, 0777);
	bool all = true;
	for (int i = 0; i < count; i++) {
		photo_path(project, shots[i].sha, path, sizeof(path));
		if (file_exists(path) || fetch_photo(project, &shots[i])) {
			memcpy(sha[n++], shots[i].sha, 41);
			publish_photos(project, sha, n, i + 1 < count);
		} else {
			all = false;
		}
	}
	publish_photos(project, sha, n, false);
	if (all) { // every screenshot is there: the folder holds these and nothing else
		write_index(project, sha, n);
		drop_others(project, sha, n);
	} else if (n) {
		write_index(project, sha, n); // (the others stay on the card for the next try)
	}
}

static int live_thread(SceSize args, void *argp) {
	(void)args;
	(void)argp;
	for (;;) {
		sceKernelWaitSema(wake, 1, NULL);
		for (int i = 0; i < source_count; i++) {
			lock();
			bool ask = live[i].ask;
			live[i].ask = false;
			unlock();
			if (ask)
				ask_project(i);
		}
	}
	return 0;
}

// ---- for the screen ----

bool live_start(const LiveSource *list, int count) {
	sources = list;
	source_count = count > PROJECTS_MAX ? PROJECTS_MAX : count;
	mutex = sceKernelCreateMutex("tweaks_live", 0, 0, NULL);
	wake = sceKernelCreateSema("tweaks_live_wake", 0, 0, 16, NULL);
	if (mutex < 0 || wake < 0)
		return false;
	for (int i = 0; i < source_count; i++) {
		char path[200];
		int len = 0;
		read_index(i);
		releases_path(i, path, sizeof(path));
		char *kept = file_read(path, ANSWER_MAX, &len);
		if (kept)
			take_releases(i, kept);
		free(kept);
	}
	SceUID thread = sceKernelCreateThread("tweaks_live", live_thread, 0x10000100, 256 * 1024, 0, 0, NULL);
	return thread >= 0 && sceKernelStartThread(thread, 0, NULL) >= 0;
}

void live_ask(void) {
	if (mutex < 0)
		return;
	bool any = false;
	lock();
	for (int i = 0; i < source_count; i++) {
		LiveState state = live[i].info.state;
		if (state == LIVE_NOT_ASKED || state == LIVE_FAILED) {
			live[i].info.state = LIVE_LOOKING; // (so asking twice in a row asks once)
			live[i].ask = true;
			any = true;
		}
	}
	if (any)
		generation++;
	unlock();
	if (any)
		sceKernelSignalSema(wake, 1);
}

void live_info(int project, LiveInfo *out) {
	memset(out, 0, sizeof(*out));
	if (mutex < 0 || project < 0 || project >= source_count)
		return;
	lock();
	*out = live[project].info;
	unlock();
}

bool live_release(int project, int index, Release *out) {
	if (mutex < 0 || project < 0 || project >= source_count || index < 0)
		return false;
	lock();
	bool there = index < live[project].info.releases;
	if (there)
		*out = live[project].release[index];
	unlock();
	return there;
}

int live_generation(void) {
	return generation;
}

uint8_t *live_photo(int project, int index) {
	char sha[41], path[200];
	if (mutex < 0 || project < 0 || project >= source_count || index < 0)
		return NULL;
	lock();
	bool there = index < live[project].info.photos;
	if (there)
		memcpy(sha, live[project].sha[index], 41);
	unlock();
	if (!there)
		return NULL;
	photo_path(project, sha, path, sizeof(path));
	int len = 0;
	char *data = file_read(path, PHOTO_BYTES, &len);
	if (data && len != PHOTO_BYTES) {
		free(data);
		data = NULL;
	}
	return (uint8_t *)data;
}
