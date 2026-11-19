// Getting the author's other projects from GitHub and putting them on the
// Vita: an app's VPK through the system's installer, a plugin's file into
// memory for the caller to place, and a big zip of data files downloaded to
// the memory card and unpacked there.
//
// Everything downloaded is checked the way the installer's own update is:
// the connection is to GitHub (net.h), and the file's SHA-256 is the one
// GitHub's release page gives for it.
#ifndef PROJECTS_H
#define PROJECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "lookup.h"
#include "update.h" // UpdateSay

typedef ReleaseFile ProjectFile;

// The file of a release of repo ("owner/name") that is called name, or, for a
// name that starts with '*', the first whose name ends with the rest. tag
// says which release; NULL or "" is the newest.
bool project_find(const char *repo, const char *tag, const char *name, ProjectFile *file, UpdateSay say);
// The file, downloaded into memory and checked: malloc'd, file->size bytes.
// NULL if it is over max, or isn't the file GitHub lists.
uint8_t *project_fetch(const ProjectFile *file, int max, UpdateSay say);
// Installs a VPK held in memory as an app, through the system's installer.
// title, if not NULL, gets the app's name from its param.sfo.
bool project_install_vpk(const uint8_t *vpk, int len, char *title, int title_size, UpdateSay say);
// Downloads a zip to the memory card, checks it, and unpacks it into the
// folder `into`. Every file in it must be under `folder/`. The zip is deleted
// afterwards.
bool project_install_zip(const ProjectFile *file, const char *folder, const char *into, UpdateSay say);

#endif
