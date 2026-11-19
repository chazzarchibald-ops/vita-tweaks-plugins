// What is on GitHub now for each of "My other projects": its releases, with
// their notes and the size of each download, and its screenshots, which are
// turned into the photos the app shows. A thread of its own asks, so the
// screen never waits for the network.
//
// Both are kept on the memory card, in ux0:data/tweaks_installer, and shown
// from there at once and when there is no network:
//   releases/<project>.json   GitHub's last answer about its releases
//   photos/<project>/         its photos, each under git's own checksum of the
//                             screenshot it was made from
// A screenshot that hasn't changed is never downloaded again; a new or
// changed one is; one that is gone from the project is dropped.
#ifndef LIVE_H
#define LIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "lookup.h"

#define LIVE_RELEASES 8 // at most, of a project: its newest
#define LIVE_PHOTOS 8
#define PHOTO_W 448
#define PHOTO_H 254

typedef struct {
	const char *key;                  // names what is kept of it
	const char *repo;                 // "owner/name"
	const char *shots;                // the folder of its repository its screenshots are in
	const char *asset[RELEASE_FILES]; // the release files to give sizes of, or NULL (see lookup_asset)
} LiveSource;

typedef enum { LIVE_NOT_ASKED, LIVE_LOOKING, LIVE_FOUND, LIVE_FAILED } LiveState;

typedef struct {
	LiveState state;     // of asking GitHub this time
	int releases;        // how many are known, from now or from before
	char release[64];    // the newest one's name (the newest that isn't a pre-release, if there is one)
	char date[16];       // and when it came out, or ""
	int photos;          // how many there are to show now
	bool getting_photos; // and more may be on their way
} LiveInfo;

// Reads what is kept, and starts the thread. The sources must stay.
bool live_start(const LiveSource *sources, int count);
// Asks GitHub about every project, or again about those it couldn't reach. Returns at once.
void live_ask(void);
// What is known of a project now.
void live_info(int project, LiveInfo *out);
// One of its releases, newest first. False if there is no such release.
bool live_release(int project, int index, Release *out);
// A number that changes whenever any of that does.
int live_generation(void);
// One of a project's photos: PHOTO_W x PHOTO_H pixels of red, green and blue,
// malloc'd. NULL if there is no such photo.
uint8_t *live_photo(int project, int index);

#endif
