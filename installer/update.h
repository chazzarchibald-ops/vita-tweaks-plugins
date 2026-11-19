// Checks GitHub for a newer Tweaks/Plugins release and, if its installer VPK is there,
// downloads it and has the system install it in this app's place, by way of
// an updater: this same program, installed under a second title ID.
//
// What a release needs for this: a tag that is a version number ("1.6") and a
// file named tweaks_installer.vpk. The VPK is checked three ways before
// anything of the app is touched: the connection is to GitHub (net.h), the
// file's SHA-256 is the one GitHub's release page gives for it, and it unpacks
// whole, with this app's title ID inside. The updater checks the last two again.
#ifndef UPDATE_H
#define UPDATE_H

#include <stdbool.h>

typedef enum { SAY_TEXT, SAY_DIM, SAY_GOOD, SAY_BAD } SayKind;
// A line for the screen. again: it takes the place of the line before (progress).
typedef void (*UpdateSay)(SayKind kind, bool again, const char *fmt, ...);

typedef struct {
	char tag[24];     // the release's version, "1.6"
	char url[320];    // where its tweaks_installer.vpk is
	char digest[65];  // that file's SHA-256, as GitHub gives it
	int size;
} UpdateInfo;

typedef enum { UPDATE_FAILED, UPDATE_NONE, UPDATE_READY } UpdateFound;

// have: the Tweaks/Plugins version this installer carries.
UpdateFound update_check(const char *have, UpdateInfo *info, UpdateSay say);
// In the installer: downloads and checks the VPK, keeps it in ux0:data, and
// installs the updater. True if the updater is ready to be opened.
bool update_install(const UpdateInfo *info, UpdateSay say);
// Opens the updater, and ends this app.
void update_open_updater(void);
// At the installer's start: deletes the updater an update left behind.
void update_tidy(void);

// True if this program is running as the updater.
bool update_is_updater(void);
// In the updater: unpacks the kept VPK and has the system install it. True if
// the installer is now the new version.
bool update_finish(UpdateSay say);
// Opens the installer, and ends this app.
void update_open_installer(void);

#endif
