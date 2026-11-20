# Notes for whoever works on this installer next

This installer is the [Engram Installer](https://github.com/chazzarchibald-ops/engram-vita/tree/main/installer)
with another plugin in it, and the two are worked on from different computers.
These notes say what was changed here from the Engram side, and why, so the
next session on either side starts informed.

## 19 November 2026: the copy that could fail quietly (commit `f9f39fd`)

### What went wrong

On a retail 3.65 Vita, Engram Installer's **Install or update Engram** left
the old plugin where it was. `ur0:tai/engram.suprx` kept its old contents and
its old date, and Settings went on showing the old version after a restart.

The copy step was the one this installer had too:

1. write the new plugin beside the old one, as `<file>.installer-new`, and read
   it back;
2. `sceIoRename` it over the old file;
3. if that is refused, `sceIoRemove` the old file and rename again.

For the old file to survive untouched, the system must have refused both the
rename and the removal. The screen then said only "Couldn't copy ...", with
nothing to say why.

**The cause was not found.** The same step had worked on that Vita two days
before. Engram is loaded by the home screen (`*main`) while the installer
runs, so its file may have been held. That is a guess. `tweaks.suprx` is only loaded by Settings, so
this installer may be less exposed, but the code was the same.

### What was changed

In `main.c`, `files.c` and `files.h`, the same change as in Engram Installer
(engram-vita commit `17a4e68`):

- `replace_file()` now takes the new contents as well. If the rename and the
  remove-then-rename are both refused, it writes the new contents over the file
  where it is (`file_write_checked`, so it is read back and compared). Writing
  in place is how the file has always been replaced over FTP on that Vita,
  with the plugin loaded. If that fails too, the old contents are written back
  as before.
- `put_file()` is the plugin's copy in one place: beside the file, into place
  with `replace_file()`, read back from there. If it can't even be written
  beside the file, it is written where it goes.
- taiHEN's `config.txt` goes through the same `replace_file()`, so it has the
  same fallback.
- `file_write_error()` (files.c) keeps the system's answer when a write fails,
  and `put_error` (main.c) the answer to the step that failed last. The two
  messages show it: "Couldn't copy tweaks.suprx to ur0:tai/tweaks.suprx
  (0x........)" and "Couldn't write the config (0x........)". If someone
  reports one of these, the code is the thing to ask for.

Renaming is still tried first: it can't leave half a file if the power goes.

### How far it was tested

- **Engram Installer:** confirmed on the console. After the change, Install
  put the new plugin in and Settings showed the new version.
- **This installer:** it builds with no new warnings. It had not been run on a
  console when this note was written.

### What was published, and what to do about it

- `main` moved from `cb2dc61` (Tweaks/Plugins 1.1) to `f9f39fd`. **Pull before
  working**, or the next push is rejected.
- The `v1.1` tag was moved from `cb2dc61` to `f9f39fd` with a forced push, so
  the tag matches the source of the VPK on the release. A plain fetch keeps
  the old tag: use `git fetch --tags --force`.
- `tweaks_installer.vpk` on the v1.1 pre-release was replaced with a build of
  `f9f39fd` (443,647 bytes; the one before was 447,113). `tweaks.suprx` on the
  release, the notes and the pre-release flag were left as they were, and the
  new VPK carries that same `tweaks.suprx`, byte for byte.
- That VPK was built on a different computer with a different VitaSDK from the
  one 1.1 was first built with. Besides `eboot.bin`, its two `param.sfo` files
  differ: this SDK's `vita-mksfoex` leaves `CONTENT_ID` empty and `ATTRIBUTE2`
  0, and writes `BOOT_FILE`, `EBOOT_*`, `LAREA_TYPE` and
  `NP_COMMUNICATION_ID` where the other wrote `GC_*`, `REGION_DENY` and
  `SAVEDATA_MAX_SIZE`. Engram Installer is built with the same SDK and
  installs, runs and updates itself on the console. The text files in it have
  Windows line ends. If the release is rebuilt on the first computer, build
  from `f9f39fd` or later.

## What the Engram side took from here

Engram Installer's **My other projects** now works as this one's does:

- it installs a project's own installer, not its plugin. For Tweaks/Plugins it
  looks, on the release chosen, for a file named exactly
  `tweaks_installer.vpk`: **keep that name on releases**;
- only a project's newest release has an Install button, and pre-releases
  count as releases. Older ones can be read about and opened on GitHub.

## Keeping the two in step

Apart from names, title IDs and folders, these files were the same in both
installers on 19 November 2026: `gfx`, `liquid`, `lookup`, `package`,
`projects`, `live`, `net`, `files`, `update`, `unzip`, `json`, `png`, `sha1`,
`sha256`, `headbin`. A fix to one of them on either side most likely belongs
on the other too. `main.c` is where they differ: the plugin, its config lines
and the menu.
