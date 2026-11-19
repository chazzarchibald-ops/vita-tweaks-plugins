// Reading what GitHub says of a project: the files of its newest release, the
// release's name and date, and the screenshots in its repository. Text in,
// facts out: nothing here goes online (net.h does) or touches a file.
#ifndef LOOKUP_H
#define LOOKUP_H

#include <stdbool.h>

// A file of a release.
typedef struct {
	char tag[24];    // the release it is from
	char name[80];   // the file's name
	char url[320];   // where it is
	char digest[65]; // its SHA-256, as GitHub gives it
	int size;
} ReleaseFile;

// The file called name in a release's answer, or, for a name that starts with
// '*', the first whose name ends with the rest. False if there is none, or
// its details aren't as expected: a size, a SHA-256, and a link under
// https://github.com/<repo>/releases/download/.
bool lookup_asset(const char *answer, const char *repo, const char *name, ReleaseFile *file);

// The release's name (its tag if it has none) in plain letters, and the day it
// came out, like "6 Nov 2026" ("" if the answer doesn't say).
bool lookup_release(const char *answer, char *name, int name_size, char date[16]);

// A size for showing: "18 KB", "14.6 MB", "422 MB".
void lookup_size_text(int bytes, char out[12]);

// A release as the app shows it.
#define RELEASE_FILES 3
#define RELEASE_NOTES 6144
#define NOTES_RAW (24 * 1024) // as much of a release's notes as is read
typedef struct {
	char tag[40];
	char name[64];
	char date[16];
	bool prerelease;
	char size[RELEASE_FILES][12]; // each file asked about, "14.6 MB", or "" if the release doesn't have it
	// Its notes as plain lines, a newline between them: a line that starts
	// with \1 is a heading, one that starts with \2 an item of a list, and an
	// empty line is a gap. Markdown's marks are taken off and only plain
	// letters are left.
	char notes[RELEASE_NOTES];
} Release;

// A repository's releases, newest first, from the answer to GitHub's
// "releases" request: up to max of them, drafts left out. assets names the
// files to give sizes of (see lookup_asset). Returns how many, or -1 if the
// answer isn't a list.
int lookup_releases(const char *answer, const char *repo, const char *const *assets, int asset_count, Release *out, int max);

// A screenshot in a repository.
typedef struct {
	char sha[41];  // of its contents, as git has it: changes when the picture does
	char url[320]; // where it is
	int size;
} Shot;

// The .png files of a folder, from the answer to GitHub's "contents" request,
// in its order, up to max of them. Only ones whose details are as expected
// are taken: a 40-digit sha and a link under
// https://raw.githubusercontent.com/<repo>/. Returns how many, or -1 if the
// answer isn't a list of files.
int lookup_shots(const char *answer, const char *repo, Shot *out, int max);

#endif
