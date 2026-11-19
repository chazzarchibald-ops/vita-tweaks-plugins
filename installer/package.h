// The system's own installer (ScePromoterUtil), which is what puts an app in
// ux0:app, makes its bubble and keeps the system's records of it. An app can't
// write to its own folder there, so this is the only way to replace one.
#ifndef PACKAGE_H
#define PACKAGE_H

#include <stdbool.h>

// Loads the installer. False, with the system's error in *error, if it won't.
bool package_start(int *error);
// Installs the app laid out in dir (its files, sce_sys/param.sfo and
// sce_sys/package/head.bin), in place of any app of the same title ID. The
// folder is taken: it is gone afterwards. 0, or the system's error.
int package_install(const char *dir);
bool package_installed(const char *titleid);
// Deletes an app that isn't running. 0, or the system's error.
int package_remove(const char *titleid);
// Opens an app, and ends this one.
void package_launch(const char *titleid);

#endif
