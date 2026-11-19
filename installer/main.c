// Tweaks/Plugins Installer: puts tweaks.suprx in ur0:tai and its line in
// taiHEN's config.txt, or takes them out again.
//
// It is the Engram Installer with another plugin in it: the screen is drawn
// by the app itself, on the CPU, a ball of liquid that swooshes around
// (liquid.c) behind glass panels and text (gfx.c) in the fonts of its
// LiveArea, which the VPK carries, so it needs nothing beyond the SDK. It
// writes to ur0:, so the VPK is built as unsafe homebrew.
//
// What it touches, and nothing else:
// - ur0:tai/tweaks.suprx (copied from app0:tweaks.suprx in the VPK);
// - the config.txt taiHEN reads (ux0:tai/config.txt if it's there, otherwise
//   ur0:tai/config.txt): Tweaks/Plugins' line, first under *NPXS10015, the
//   Settings app, as its README asks, and taken out of anywhere else it was.
//   Before each change the old file is copied beside it as
//   config.txt.tweaks-installer-old, and the first time also to
//   ur0:data/tweaks/tai_config_before_installer.txt;
// - ur0:data/tweaks, made if it's missing, and the failsafe's markers there
//   (disabled, noprobe, building, strike), removed on install.
// "Check for a new version" asks GitHub (update.c). With a newer release that
// has an installer, START downloads its VPK, keeps a copy at
// ux0:data/tweaks_installer.vpk and has the system install it in this app's
// place. Nothing is sent but the request itself.
// Uninstalling takes every Tweaks/Plugins line out of config.txt and deletes
// ur0:tai/tweaks.suprx. Its settings and logs in ur0:data/tweaks stay.

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "files.h"
#include "gfx.h"
#include "live.h"
#include "projects.h"
#include "update.h"

#define INSTALLER_VERSION "1.0"
#ifdef UPDATE_TEST_TAG
#define VERSION_TEXT "Update test  -  Installer " INSTALLER_VERSION "  -  Tweaks/Plugins " TWEAKS_VERSION
#else
#define VERSION_TEXT "Installer " INSTALLER_VERSION "  -  Tweaks/Plugins " TWEAKS_VERSION
#endif

// Room for a 15 MB VPK and one of its files unpacked at a time, on top of the
// network libraries' own memory: more than the 32 MB a program gets unasked.
unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;

#define BUNDLED_PLUGIN "app0:tweaks.suprx"
#define PLUGIN_PATH "ur0:tai/tweaks.suprx"
#define PLUGIN_LINE "ur0:tai/tweaks.suprx"
#define TAI_UX0 "ux0:tai/config.txt" // taiHEN reads this one if it's there,
#define TAI_UR0 "ur0:tai/config.txt" // and this one otherwise
#define DATA_DIR "ur0:data/tweaks"
#define FIRST_BACKUP DATA_DIR "/tai_config_before_installer.txt"
#define CONFIG_MAX (256 * 1024)
#define PLUGIN_MAX (4 * 1024 * 1024)

#define COLOR_TEXT RGB(0xEC, 0xEC, 0xF0)
#define COLOR_DIM RGB(0xA8, 0xAE, 0xBE)
#define COLOR_ACCENT RGB(0x7C, 0xC0, 0xFF)
#define COLOR_GOOD RGB(0x76, 0xE0, 0x96)
#define COLOR_BAD RGB(0xFF, 0x84, 0x84)
#define COLOR_PANEL RGB(10, 12, 20)

// ---- enter and back ----
// Which of O and X enters and which goes back is the system's setting: O
// enters on a Japanese Vita and X everywhere else, and HENkaku Settings can
// swap them (its "O Button Behavior" is this same setting).
static unsigned btn_enter = SCE_CTRL_CROSS, btn_back = SCE_CTRL_CIRCLE;
static const char *enter_name = "X", *back_name = "O";

static void read_buttons(void) {
	SceAppUtilInitParam init;
	SceAppUtilBootParam boot;
	memset(&init, 0, sizeof(init));
	memset(&boot, 0, sizeof(boot));
	sceAppUtilInit(&init, &boot);
	int assign = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &assign) >= 0 &&
	    assign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
		btn_enter = SCE_CTRL_CIRCLE;
		btn_back = SCE_CTRL_CROSS;
		enter_name = "O";
		back_name = "X";
	}
}

// ---- the log on screen ----

#define LOG_LINES 13
static char log_text[LOG_LINES][128];
static uint32_t log_color[LOG_LINES];
static int log_count;

static void log_add(uint32_t color, const char *fmt, ...) {
	if (log_count == LOG_LINES) {
		memmove(log_text, log_text[1], sizeof(log_text[0]) * (LOG_LINES - 1));
		memmove(log_color, log_color + 1, sizeof(log_color[0]) * (LOG_LINES - 1));
		log_count--;
	}
	va_list args;
	va_start(args, fmt);
	vsnprintf(log_text[log_count], sizeof(log_text[0]), fmt, args);
	va_end(args);
	log_color[log_count++] = color;
}

static void log_drop_last(void) {
	if (log_count)
		log_count--;
}

static void log_clear(void) {
	log_count = 0;
}

// ---- files ----

// Puts new_path in place of path: a rename, and where it can't replace a
// file, path is removed first and, if the rename still fails, old is written
// back.
static bool replace_file(const char *path, const char *new_path, const char *old, int old_len) {
	if (sceIoRename(new_path, path) >= 0)
		return true;
	sceIoRemove(path);
	if (sceIoRename(new_path, path) >= 0)
		return true;
	if (old)
		file_write(path, old, old_len);
	return false;
}

// ---- taiHEN's config.txt ----

typedef struct {
	char **line; // without their line ends
	int count, cap;
	bool crlf; // the file's own line ends
	bool final_newline;
} Lines;

static void lines_free(Lines *ls) {
	for (int i = 0; i < ls->count; i++)
		free(ls->line[i]);
	free(ls->line);
	memset(ls, 0, sizeof(*ls));
}

// Takes the first len characters of s as the new line at.
static bool lines_insert_n(Lines *ls, int at, const char *s, size_t len) {
	if (ls->count == ls->cap) {
		int cap = ls->cap ? ls->cap * 2 : 64;
		char **grown = realloc(ls->line, sizeof(char *) * cap);
		if (!grown)
			return false;
		ls->line = grown;
		ls->cap = cap;
	}
	char *copy = malloc(len + 1);
	if (!copy)
		return false;
	memcpy(copy, s, len);
	copy[len] = '\0';
	memmove(ls->line + at + 1, ls->line + at, sizeof(char *) * (ls->count - at));
	ls->line[at] = copy;
	ls->count++;
	return true;
}

static bool lines_insert(Lines *ls, int at, const char *s) {
	return lines_insert_n(ls, at, s, strlen(s));
}

static void lines_remove(Lines *ls, int at) {
	free(ls->line[at]);
	memmove(ls->line + at, ls->line + at + 1, sizeof(char *) * (ls->count - at - 1));
	ls->count--;
}

static bool lines_parse(Lines *ls, const char *text) {
	memset(ls, 0, sizeof(*ls));
	ls->crlf = strstr(text, "\r\n") != NULL;
	size_t n = strlen(text);
	ls->final_newline = n > 0 && text[n - 1] == '\n';
	const char *p = text;
	while (*p) {
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len && p[len - 1] == '\r')
			len--;
		if (!lines_insert_n(ls, ls->count, p, len))
			return false;
		if (!end)
			break;
		p = end + 1;
	}
	return true;
}

static char *lines_join(const Lines *ls, int *len) {
	const char *eol = ls->crlf ? "\r\n" : "\n";
	size_t total = 1;
	for (int i = 0; i < ls->count; i++)
		total += strlen(ls->line[i]) + 2;
	char *out = malloc(total);
	if (!out)
		return NULL;
	size_t at = 0;
	for (int i = 0; i < ls->count; i++) {
		size_t l = strlen(ls->line[i]);
		memcpy(out + at, ls->line[i], l);
		at += l;
		if (i + 1 < ls->count || ls->final_newline) {
			memcpy(out + at, eol, strlen(eol));
			at += strlen(eol);
		}
	}
	out[at] = '\0';
	*len = (int)at;
	return out;
}

// The line with its surrounding spaces taken off, in buf.
static const char *trimmed(const char *s, char *buf, size_t size) {
	while (*s == ' ' || *s == '\t')
		s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		n--;
	if (n >= size)
		n = size - 1;
	memcpy(buf, s, n);
	buf[n] = '\0';
	return buf;
}

static bool ends_with_nocase(const char *s, const char *tail) {
	size_t a = strlen(s), b = strlen(tail);
	return a >= b && strcasecmp(s + a - b, tail) == 0;
}

static bool is_plugin_line(const char *line) {
	char t[512];
	trimmed(line, t, sizeof(t));
	return t[0] && t[0] != '#' && ends_with_nocase(t, "/tweaks.suprx");
}

static bool is_section(const char *line, const char *name) {
	char t[512];
	trimmed(line, t, sizeof(t));
	return t[0] == '*' && strcasecmp(t + 1, name) == 0;
}

// The first line of a section, its header's index, or -1.
static int section_start(const Lines *ls, const char *name) {
	for (int i = 0; i < ls->count; i++)
		if (is_section(ls->line[i], name))
			return i;
	return -1;
}

static const char *tai_config_path(void) {
	return file_exists(TAI_UX0) ? TAI_UX0 : TAI_UR0;
}

// Reads taiHEN's config, changes it with edit(), and writes it back safely.
// unchanged: what to say when edit() had nothing to do.
static bool change_config(bool (*edit)(Lines *), int *changes, const char *unchanged) {
	const char *path = tai_config_path();
	int len = 0;
	char *old = file_read(path, CONFIG_MAX, &len);
	if (!old) {
		log_add(COLOR_BAD, "Couldn't read %s.", path);
		return false;
	}
	log_add(COLOR_DIM, "taiHEN's config: %s", path);
	Lines ls;
	if (!lines_parse(&ls, old)) {
		free(old);
		log_add(COLOR_BAD, "Out of memory reading the config.");
		return false;
	}
	bool ok = edit(&ls);
	int new_len = 0;
	char *text_new = ok ? lines_join(&ls, &new_len) : NULL;
	*changes = text_new ? (new_len != len || memcmp(text_new, old, len) != 0) : 0;
	lines_free(&ls);
	if (!text_new) {
		free(old);
		log_add(COLOR_BAD, "Couldn't change the config.");
		return false;
	}
	if (!*changes) {
		free(old);
		free(text_new);
		log_add(COLOR_DIM, "%s", unchanged);
		return true;
	}
	sceIoMkdir(DATA_DIR, 0777);
	if (!file_exists(FIRST_BACKUP))
		file_write(FIRST_BACKUP, old, len);
	char side[96], fresh[96];
	snprintf(side, sizeof(side), "%s.tweaks-installer-old", path);
	snprintf(fresh, sizeof(fresh), "%s.tweaks-installer-new", path);
	bool done = file_write_checked(side, old, len) && file_write_checked(fresh, text_new, new_len) &&
	            replace_file(path, fresh, old, len);
	if (done) {
		int back_len = 0;
		char *back = file_read(path, CONFIG_MAX, &back_len);
		done = back && back_len == new_len && memcmp(back, text_new, new_len) == 0;
		free(back);
	}
	if (done)
		log_add(COLOR_GOOD, "Config updated. The old one is %s", side);
	else
		log_add(COLOR_BAD, "Couldn't write the config: it was left as it was.");
	sceIoRemove(fresh);
	free(old);
	free(text_new);
	return done;
}

// Tweaks/Plugins' line: the first under the Settings app's section, as its
// README asks, and nowhere else. Any line of its own is taken out first, so
// one under *NPXS10016, the Settings pop-up, where it does nothing, goes too.
static bool edit_install(Lines *ls) {
	for (int i = 0; i < ls->count;)
		if (is_plugin_line(ls->line[i]))
			lines_remove(ls, i);
		else
			i++;
	int header = section_start(ls, "NPXS10015");
	if (header < 0) {
		if (ls->count && ls->line[ls->count - 1][0])
			lines_insert(ls, ls->count, "");
		return lines_insert(ls, ls->count, "*NPXS10015") && lines_insert(ls, ls->count, PLUGIN_LINE);
	}
	return lines_insert(ls, header + 1, PLUGIN_LINE);
}

static bool edit_uninstall(Lines *ls) {
	for (int i = 0; i < ls->count;)
		if (is_plugin_line(ls->line[i]))
			lines_remove(ls, i);
		else
			i++;
	return true;
}

// ---- install and uninstall ----

// What Tweaks/Plugins' failsafe leaves in its folder when it has turned
// itself, or its look at the other Settings pages, off.
static const char *const FAILSAFE[] = {"disabled", "noprobe", "building", "strike"};
#define FAILSAFE_COUNT 4

static bool install(void) {
	log_clear();
	int len = 0;
	char *plugin = file_read(BUNDLED_PLUGIN, PLUGIN_MAX, &len);
	if (!plugin) {
		log_add(COLOR_BAD, "tweaks.suprx is missing from this installer.");
		return false;
	}
	sceIoMkdir("ur0:tai", 0777);
	bool copied = file_write_checked(PLUGIN_PATH ".installer-new", plugin, len) &&
	              replace_file(PLUGIN_PATH, PLUGIN_PATH ".installer-new", NULL, 0);
	if (copied) {
		int back_len = 0;
		char *back = file_read(PLUGIN_PATH, PLUGIN_MAX, &back_len);
		copied = back && back_len == len && memcmp(back, plugin, len) == 0;
		free(back);
	}
	free(plugin);
	sceIoRemove(PLUGIN_PATH ".installer-new");
	if (!copied) {
		log_add(COLOR_BAD, "Couldn't copy tweaks.suprx to " PLUGIN_PATH ".");
		return false;
	}
	log_add(COLOR_GOOD, "Copied tweaks.suprx (%d bytes) to " PLUGIN_PATH ".", len);

	int changes = 0;
	if (!change_config(edit_install, &changes, "The config had Tweaks/Plugins first under *NPXS10015 already."))
		return false;
	log_add(COLOR_TEXT, "Tweaks/Plugins is first under *NPXS10015, the Settings app.");

	sceIoMkdir(DATA_DIR, 0777);
	bool was_off = file_exists(DATA_DIR "/disabled");
	for (int i = 0; i < FAILSAFE_COUNT; i++) {
		char path[64];
		snprintf(path, sizeof(path), DATA_DIR "/%s", FAILSAFE[i]);
		sceIoRemove(path);
	}
	if (was_off)
		log_add(COLOR_TEXT, "Tweaks/Plugins was turned off by its failsafe: it's on again.");
	log_add(COLOR_GOOD, "Done. Restart the Vita, then open Settings.");
	return true;
}

static bool uninstall(void) {
	log_clear();
	int changes = 0;
	if (!change_config(edit_uninstall, &changes, "The config had no Tweaks/Plugins line."))
		return false;
	if (file_exists(PLUGIN_PATH)) {
		if (sceIoRemove(PLUGIN_PATH) < 0) {
			log_add(COLOR_BAD, "Couldn't delete " PLUGIN_PATH ".");
			return false;
		}
		log_add(COLOR_GOOD, "Deleted " PLUGIN_PATH ".");
	}
	log_add(COLOR_TEXT, "Your settings and logs stay in " DATA_DIR ".");
	log_add(COLOR_GOOD, "Done. Restart the Vita to finish.");
	return true;
}

// ---- the screen ----

enum { VIEW_MENU, VIEW_WORKING, VIEW_RESULT, VIEW_WINDOW };

enum { MENU_INSTALL, MENU_UPDATE, MENU_UNINSTALL, MENU_PROJECTS, MENU_EXIT };
static const char *const MENU[] = {"Install or update", "Check for a new version", "Uninstall", "My other projects",
                                   "Exit"};
static const char *const MENU_HELP[] = {
	"Copies Tweaks/Plugins to ur0:tai and puts it first under *NPXS10015 in taiHEN's config.txt. A copy of the config "
	"is kept first.",
	"Asks GitHub whether a newer Tweaks/Plugins is out. If it is, this installer can download it and update itself.",
	"Takes Tweaks/Plugins out of config.txt and deletes the plugin. Your settings and logs stay.",
	"More for the PS Vita by the author of Tweaks/Plugins. Select to see each one, and to install it from here.",
	"Leaves without changing anything.",
};
#define MENU_COUNT 5

static const char *const FEATURES[][2] = {
	{"One menu", "Every plugin's settings page, under HENkaku Settings"},
	{"Finds them itself", "A new plugin shows up the next time Settings opens"},
	{"Your choice", "Switch each plugin between here and its own place"},
	{"taiHEN Plugins", "Turn the plugins in your config on or off, and reboot"},
	{"Safe", "If Settings ever crashes on it, it steps out of the way"},
};
#define FEATURE_COUNT 5

// The author's other projects for the Vita, shown under "My other projects":
// a card each beside the menu, and a window with more: its photos and what it
// does, and behind its Download button its releases, each with its notes and
// the buttons that install it.
typedef struct {
	const char *name, *kind; // kind: "Plugin" or "App"
	uint32_t color;          // its emblem's: a ball of this colour
	const char *letter;      // with this letter on it,
	const char *icon;        // or its bubble's own picture: app0:icons/<this>_<size>.rgb
	const char *about;       // two lines at most, for its card
	const char *where;       // its page, as shown: the same address without "https://"
	const char *what[6];     // what it does, for the window
	int what_count;
	bool has_data; // its releases come with data files, which a switch leaves out
} Project;
static const Project PROJECTS[] = {
	{"Engram", "Plugin", RGB(0x2A, 0x7C, 0xFF), "E", "engram",
	 "Home screen tweaks: scroll animations, Motion Depth, 3D icon layouts and your own fonts.",
	 "github.com/chazzarchibald-ops/engram-vita",
	 {"Scroll Animations: 15 effects for the icons as you scroll between the home screen's pages.",
	  "Motion Depth: tilt the Vita and the icons float above the wallpaper.",
	  "Icon Placement: open book, sphere and inside-a-sphere layouts.",
	  "App Page Animation: effects as you swipe between your open apps.",
	  "Fonts: your own fonts for the home screen, the lock screen and apps.",
	  "Install puts the Engram Installer on the home screen: an app of its own, like this one, that installs "
	  "Engram and keeps it up to date."},
	 6, false},
	{"CyberFlow Launcher", "App", RGB(0xFC, 0xE4, 0x2A), "C", "cyberflow",
	 "A Cyberpunk 2077 styled game launcher, built on RetroFlow's 3D coverflow library.",
	 "github.com/chazzarchibald-ops/CyberFLow-Launcher",
	 {"A Cyberpunk 2077 inspired mod of RetroFlow Launcher: its 3D coverflow and its retro game library with no "
	  "bubbles needed, in a Cyberpunk 2077 interface.",
	  "Animated video backgrounds and music, different for each theme.",
	  "10 theme presets, plus Classic and a slot for your own: Arasaka, Dogtown, Night City, Edgerunners and more.",
	  "A Cyberdeck RAM header, a scanner-style game information screen, and a settings hub like the game's pause "
	  "menu.",
	  "Everything RetroFlow does: categories, collections, favourites, search, cover downloads and 20 languages.",
	  "The data files are the themes' videos and music. They go in ux0:data/RetroFlow."},
	 6, true},
};
#define PROJECT_COUNT 2
#define PROJECT_ENGRAM 0
#define PROJECT_CYBERFLOW 1
#define ENGRAM_REPO "chazzarchibald-ops/engram-vita"
#define ENGRAM_VPK "engram_installer.vpk" // Engram's own installer: it is what gets installed, not the plugin
#define CYBERFLOW_REPO "chazzarchibald-ops/CyberFLow-Launcher"
#define CYBERFLOW_VPK "*.vpk" // its name has the version in it
#define CYBERFLOW_DATA "CyberFlow_data.zip"
// What is looked up on GitHub for each, in the background (live.h): its
// releases, the files of a release that Install and Data files download (in
// that order), and the folder its screenshots are in, which its photos are
// made from.
static const LiveSource LIVE[PROJECT_COUNT] = {
	{"engram", ENGRAM_REPO, "screenshots", {ENGRAM_VPK, NULL, NULL}},
	{"cyberflow", CYBERFLOW_REPO, "Media/Screenshots", {CYBERFLOW_VPK, CYBERFLOW_DATA, NULL}},
};

// The window has two levels: the projects, and behind a project's Download
// button its releases. Each has names on the left and, on the right, something
// that scrolls over a row of buttons.
enum { LEVEL_PROJECTS, LEVEL_RELEASES };
static int win_level;
// At either level: whether the buttons act on the right side, and an install
// under way (its lines are in the log, over the right side).
static bool win_side;
static bool win_job, job_running, job_ok, job_restart;
// The projects: the one chosen, the button chosen on its right side, how far
// that side is scrolled, and the photo showing.
static int win_project, win_action, win_scroll, win_scroll_max, win_photo;
// Its releases: the one chosen, the first shown of the list, and the same of its right side.
static int rel_index, rel_top, rel_action, rel_scroll, rel_scroll_max;
static bool data_files = true; // the switch: a release's data files are downloaded too
static bool page_failed;       // the browser wouldn't open
// The buttons of a right side.
enum { ACT_DOWNLOAD, ACT_PAGE, ACT_INSTALL, ACT_DATA, ACT_RELEASE_PAGE };
typedef struct {
	int kind;
	char label[40];
	char size[12]; // the download's, as the release lists it, or ""
} Button;
#define BUTTONS_MAX 3
// Which of the two looks the kept copy of the overlay has (gfx_save).
enum { CHROME_NONE, CHROME_MAIN, CHROME_WINDOW };
static int chrome_kind;

// What the screen after "Working..." is showing, which decides its heading and buttons.
typedef enum {
	RESULT_PLUGIN,       // the plugin was installed or removed: START restarts the Vita
	RESULT_NOTE,         // something to read
	RESULT_UPDATE_READY, // a newer release: START downloads it
	RESULT_UPDATED,      // the new version is downloaded and the updater installed: START opens it
} ResultKind;
static ResultKind result_kind;
static bool updater; // this program is running as the updater (update.h)

#define PANEL_ALPHA 92
#define LEFT_X 32
#define RIGHT_X 456
#define PANEL_Y 132
#define PANEL_H 316
#define ROW_Y (PANEL_Y + 50) // the menu's rows
#define ROW_H 33
#define ROW_TEXT 19.0f
#define CARD_X (RIGHT_X + 16) // a project's card
#define CARD_W 440
#define CARD_Y (PANEL_Y + 52)
#define CARD_H 112
#define CARD_GAP 10
// The projects' window
#define WIN_Y 84
#define WIN_H 364
#define LIST_X 32
#define LIST_W 250
#define SIDE_X 298
#define SIDE_W 630
#define PANE_X (SIDE_X + 16) // what scrolls
#define PANE_W 598
#define PANE_Y (WIN_Y + 14)
#define PANE_H 284
#define BUTTON_Y (WIN_Y + WIN_H - 54)
#define BUTTON_H 40
#define RELEASE_ROWS 5 // releases that show at once

// A project's emblem, r across from its middle: its bubble's own picture cut
// to a circle, where the VPK carries one that size, or a ball in its colour,
// lit from the upper left, with its letter.
static void emblem(int project, int cx, int cy, int r, float letter_size) {
	static struct {
		int project, r;
		uint8_t *pixels;
	} kept[4];
	static int kept_count;
	const Project *p = &PROJECTS[project];
	const uint8_t *pixels = NULL;
	for (int i = 0; i < kept_count; i++)
		if (kept[i].project == project && kept[i].r == r)
			pixels = kept[i].pixels;
	if (!pixels && p->icon && kept_count < 4) {
		char path[80];
		snprintf(path, sizeof(path), "app0:icons/%s_%d.rgb", p->icon, r * 2);
		int len = 0;
		char *data = file_read(path, r * r * 12, &len);
		if (data && len == r * r * 12) {
			kept[kept_count].project = project;
			kept[kept_count].r = r;
			kept[kept_count++].pixels = (uint8_t *)data;
			pixels = (uint8_t *)data;
		} else {
			free(data);
		}
	}
	if (pixels) {
		gfx_image(cx - r, cy - r, r * 2, r * 2, r, pixels);
		return;
	}
	gfx_dot(cx, cy, r, p->color, 255);
	gfx_dot(cx - (r * 29) / 100, cy - (r * 33) / 100, (r * 46) / 100, RGB(255, 255, 255), 76);
	gfx_font(FONT_TITLE);
	gfx_text(cx - gfx_text_width(letter_size, p->letter) / 2, cy - (int)(letter_size * 0.68f), letter_size, COLOR_PANEL,
	         255, p->letter);
	gfx_font(FONT_TEXT);
}

// The other projects, in the right panel: a card each, the chosen one lit (-1: none).
static void render_projects(int chosen) {
	gfx_text(RIGHT_X + 24, PANEL_Y + 18, 16.0f, COLOR_ACCENT, 255, "My other projects");
	const char *more = "More for the PS Vita";
	gfx_text(RIGHT_X + 448 - gfx_text_width(13.0f, more), PANEL_Y + 21, 13.0f, COLOR_DIM, 255, more);
	for (int i = 0; i < PROJECT_COUNT; i++) {
		const Project *p = &PROJECTS[i];
		int x = CARD_X, y = CARD_Y + i * (CARD_H + CARD_GAP);
		bool on = i == chosen;
		gfx_panel(x, y, CARD_W, CARD_H, 14, on ? COLOR_ACCENT : RGB(255, 255, 255), on ? 64 : 14);
		emblem(i, x + 40, y + 40, 24, 28.0f);
		gfx_text(x + 78, y + 12, 19.0f, COLOR_TEXT, 255, p->name);
		gfx_text(x + CARD_W - 18 - gfx_text_width(12.5f, p->kind), y + 17, 12.5f, COLOR_ACCENT, 255, p->kind);
		gfx_text_wrap(x + 78, y + 39, CARD_W - 78 - 18, 13.5f, 17, COLOR_DIM, p->about);
		gfx_text(x + 18, y + 84, 13.0f, on ? COLOR_TEXT : COLOR_ACCENT, 255, p->where);
	}
}

// The overlay: the title, two glass panels (the menu, and what Tweaks/Plugins does or
// what just happened) and the buttons along the bottom. The panels and title
// never change, so they're drawn once and kept.
static void render_ui(int view, int selected, bool installed, bool ok, const char *perf) {
	if (chrome_kind == CHROME_MAIN) {
		gfx_restore();
	} else {
		gfx_begin();
		gfx_font(FONT_TITLE);
		gfx_text_shadow(40, 20, 54.0f, COLOR_TEXT, "Tweaks/Plugins");
		gfx_font(FONT_TEXT);
		gfx_text_shadow(44, 90, 19.0f, COLOR_DIM, "Your plugins' settings in one place");
		const char *version = VERSION_TEXT;
		gfx_text_shadow(SCREEN_W - 40 - gfx_text_width(16.0f, version), 42, 16.0f, COLOR_DIM, version);
		gfx_panel(LEFT_X, PANEL_Y, 404, PANEL_H, 20, COLOR_PANEL, PANEL_ALPHA);
		gfx_panel(RIGHT_X, PANEL_Y, 472, PANEL_H, 20, COLOR_PANEL, PANEL_ALPHA);
		gfx_panel(LEFT_X, 464, 896, 46, 14, COLOR_PANEL, PANEL_ALPHA);
		gfx_save();
		chrome_kind = CHROME_MAIN;
	}

	// left: the menu
	gfx_dot(LEFT_X + 30, PANEL_Y + 31, 5, installed ? COLOR_GOOD : COLOR_DIM, 255);
	gfx_text(LEFT_X + 46, PANEL_Y + 19, 16.0f, COLOR_DIM, 255,
	         installed ? "Tweaks/Plugins is installed" : "Tweaks/Plugins isn't installed yet");
	for (int i = 0; i < MENU_COUNT; i++) {
		int y = ROW_Y + i * ROW_H;
		bool on = i == selected && view == VIEW_MENU;
		if (on)
			gfx_panel(LEFT_X + 14, y - 4, 376, 31, 11, COLOR_ACCENT, 64);
		gfx_text(LEFT_X + 32, y, ROW_TEXT, on ? COLOR_TEXT : COLOR_DIM, 255, MENU[i]);
	}
	gfx_text_wrap(LEFT_X + 32, PANEL_Y + 254, 344, 14.0f, 19, COLOR_DIM, MENU_HELP[selected]);

	// right: what Tweaks/Plugins does, the other projects, or what just happened
	if (view == VIEW_MENU && selected == MENU_PROJECTS) {
		render_projects(-1);
	} else if (view == VIEW_MENU) {
		gfx_text(RIGHT_X + 24, PANEL_Y + 18, 16.0f, COLOR_ACCENT, 255, "What Tweaks/Plugins does");
		for (int i = 0; i < FEATURE_COUNT; i++) {
			int y = PANEL_Y + 50 + i * 46;
			gfx_text(RIGHT_X + 24, y, 19.0f, COLOR_TEXT, 255, FEATURES[i][0]);
			gfx_text(RIGHT_X + 24, y + 24, 13.5f, COLOR_DIM, 255, FEATURES[i][1]);
		}
		gfx_text(RIGHT_X + 24, PANEL_Y + 284, 14.0f, COLOR_ACCENT, 255, "It's in Settings, under HENkaku Settings.");
	} else {
		bool working = view == VIEW_WORKING;
		bool offer = !working && result_kind == RESULT_UPDATE_READY;
		bool ready = !working && result_kind == RESULT_UPDATED;
		const char *heading = working ? "Working..."
		                      : offer ? "A new version"
		                      : ready ? "Ready to install"
		                      : ok    ? "Done"
		                              : "Something went wrong";
		gfx_text(RIGHT_X + 24, PANEL_Y + 18, 16.0f,
		         working || offer || ready ? COLOR_ACCENT : ok ? COLOR_GOOD : COLOR_BAD, 255, heading);
		int y = PANEL_Y + 52;
		for (int i = 0; i < log_count && y < PANEL_Y + PANEL_H - 24; i++)
			y = gfx_text_wrap(RIGHT_X + 24, y, 424, 14.0f, 18, log_color[i], log_text[i]) + 4;
	}

	// the buttons
	static const char *const RESULT_START[] = {
		"START: restart the Vita now        ",
		"",
		"START: download and install it        ",
		"START: install it now        ",
	};
	char hint[128];
	if (view == VIEW_RESULT && updater)
		snprintf(hint, sizeof(hint), "%s: close", back_name);
	else if (view == VIEW_RESULT)
		snprintf(hint, sizeof(hint), "%s%s: back to the menu", RESULT_START[result_kind], back_name);
	else if (view == VIEW_WORKING)
		snprintf(hint, sizeof(hint), "One moment...");
	else
		snprintf(hint, sizeof(hint), "Up / Down: choose        %s: select        START: exit", enter_name);
	gfx_text(LEFT_X + 24, 476, 16.0f, COLOR_DIM, 255, hint);
	if (perf && perf[0])
		gfx_text(LEFT_X + 896 - 24 - gfx_text_width(16.0f, perf), 476, 16.0f, COLOR_DIM, 255, perf);
	gfx_end();
}

static void render_window(void);

// While the app is busy the main loop isn't drawing, so each line update.c
// has to say is put on screen at once: over the liquid as it was.
static int busy_selected;
static double busy_clock;

static void say(SayKind kind, bool again, const char *fmt, ...) {
	static const uint32_t colors[] = {COLOR_TEXT, COLOR_DIM, COLOR_GOOD, COLOR_BAD};
	char line[128];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	if (again)
		log_drop_last();
	log_add(colors[kind], "%s", line);
	if (win_job)
		render_window();
	else
		render_ui(VIEW_WORKING, busy_selected, file_exists(PLUGIN_PATH), false, "");
	gfx_show();
	liquid_frame(gfx_back(), gfx_back_index(), busy_clock, true, gfx_layers());
	gfx_present();
}

// ---- the projects' window ----

// A project's photo, from the ones kept on the memory card. The last one
// asked for stays in memory until the photos change.
static const uint8_t *photo_get(int of, int index) {
	static uint8_t *pixels;
	static int have_of = -1, have_index = -1, have_generation = -1;
	int generation = live_generation();
	if (have_of == of && have_index == index && have_generation == generation)
		return pixels;
	free(pixels);
	pixels = live_photo(of, index);
	have_of = of;
	have_index = index;
	have_generation = generation;
	return pixels;
}

// The buttons of the right side showing: a project's, or a release's.
// release is the one chosen at that level, or NULL if there is none.
static int window_buttons(const Release *release, Button *b) {
	const Project *p = &PROJECTS[win_project];
	int n = 0;
	memset(b, 0, sizeof(Button) * BUTTONS_MAX);
	if (win_level == LEVEL_PROJECTS) {
		b[n].kind = ACT_DOWNLOAD;
		snprintf(b[n++].label, sizeof(b[0].label), "Download");
		b[n].kind = ACT_PAGE;
		snprintf(b[n++].label, sizeof(b[0].label), "Open its page");
		return n;
	}
	if (!release)
		return 0;
	// Only the newest release installs from here: an older one can be read about,
	// and got from its page.
	if (rel_index == 0 && release->size[0][0]) { // and it has the file Install wants
		b[n].kind = ACT_INSTALL;
		snprintf(b[n].label, sizeof(b[0].label), "Install");
		snprintf(b[n++].size, sizeof(b[0].size), "%s", release->size[0]);
		if (p->has_data && release->size[1][0]) {
			b[n].kind = ACT_DATA;
			snprintf(b[n].label, sizeof(b[0].label), "Data files: %s", data_files ? "On" : "Off");
			snprintf(b[n++].size, sizeof(b[0].size), "%s", release->size[1]);
		}
	}
	b[n].kind = ACT_RELEASE_PAGE;
	snprintf(b[n++].label, sizeof(b[0].label), "Open release page");
	return n;
}

// s cut short with "..." if it is wider than width at this size.
static void fit_text(char *s, float size, int width) {
	int len = (int)strlen(s);
	if (gfx_text_width(size, s) <= width)
		return;
	while (len > 1) {
		len--;
		memcpy(s + len, "...", 4);
		if (gfx_text_width(size, s) <= width)
			return;
	}
}

// How far down a right side is: a thin bar at its edge.
static void scroll_bar(int scroll, int max) {
	if (max <= 0)
		return;
	int bar = PANE_H * PANE_H / (PANE_H + max);
	if (bar < 24)
		bar = 24;
	gfx_panel(PANE_X + PANE_W + 6, PANE_Y, 4, PANE_H, 2, RGB(255, 255, 255), 26);
	gfx_panel(PANE_X + PANE_W + 6, PANE_Y + (PANE_H - bar) * scroll / max, 4, bar, 2, COLOR_ACCENT, 190);
}

// A project's right side: a photo, its newest release, and what it does.
static void pane_project(const LiveInfo *info) {
	const Project *p = &PROJECTS[win_project];
	int y = PANE_Y - win_scroll, px = PANE_X + (PANE_W - PHOTO_W) / 2;
	const uint8_t *pixels = info->photos ? photo_get(win_project, win_photo) : NULL;
	if (pixels) {
		gfx_image(px, y, PHOTO_W, PHOTO_H, 10, pixels);
	} else { // an empty frame, which says why
		const char *why = info->getting_photos ? "Getting its photos from GitHub..."
		                                       : "Its photos come from GitHub when the Vita is online.";
		gfx_panel(px, y, PHOTO_W, PHOTO_H, 10, RGB(24, 28, 44), 255);
		gfx_text(px + (PHOTO_W - gfx_text_width(14.0f, why)) / 2, y + PHOTO_H / 2 - 10, 14.0f, COLOR_DIM, 255, why);
	}
	y += PHOTO_H + 8;
	if (info->photos) {
		char caption[48];
		snprintf(caption, sizeof(caption), "Photo %d of %d%s", win_photo + 1, info->photos,
		         info->getting_photos ? ", more coming" : "");
		gfx_text(px, y, 13.0f, COLOR_DIM, 255, caption);
	}
	if (info->photos > 1) {
		const char *more = "L / R: more photos";
		gfx_text(px + PHOTO_W - gfx_text_width(13.0f, more), y, 13.0f, COLOR_DIM, 255, more);
	}
	y += 28;
	// the newest release, as GitHub has it now (or had it, the last time it was reached)
	char newest[112];
	if (info->releases && info->date[0])
		snprintf(newest, sizeof(newest), "%s  (%s)", info->release, info->date);
	else if (info->releases)
		snprintf(newest, sizeof(newest), "%s", info->release);
	else
		snprintf(newest, sizeof(newest), "%s",
		         info->state == LIVE_FAILED  ? "GitHub couldn't be reached"
		         : info->state == LIVE_FOUND ? "None yet"
		                                     : "Asking GitHub...");
	gfx_text(PANE_X + 8, y, 16.0f, COLOR_ACCENT, 255, "Newest release");
	gfx_text(PANE_X + 8 + gfx_text_width(16.0f, "Newest release") + 14, y + 2, 14.0f,
	         info->releases ? COLOR_TEXT : COLOR_DIM, 255, newest);
	y += 34;
	gfx_text(PANE_X + 8, y, 16.0f, COLOR_ACCENT, 255, "What it does");
	y += 28;
	for (int i = 0; i < p->what_count; i++) {
		gfx_dot(PANE_X + 13, y + 10, 2, COLOR_ACCENT, 255);
		y = gfx_text_wrap(PANE_X + 26, y, PANE_W - 40, 14.0f, 19, COLOR_TEXT, p->what[i]) + 6;
	}
	win_scroll_max = y + 6 + win_scroll - PANE_Y - PANE_H;
	if (win_scroll_max < 0)
		win_scroll_max = 0;
	scroll_bar(win_scroll, win_scroll_max);
}

// A release's right side: its name, when it came out, and its notes.
static void pane_release(const LiveInfo *info, const Release *release) {
	if (!release) {
		const char *why = info->state == LIVE_FAILED  ? "GitHub couldn't be reached, and no releases are kept from before."
		                  : info->state == LIVE_FOUND ? "This project has no releases yet."
		                                              : "Asking GitHub for its releases...";
		gfx_text(PANE_X + 8, PANE_Y + 4, 15.0f, COLOR_DIM, 255, why);
		rel_scroll_max = 0;
		return;
	}
	int y = PANE_Y - rel_scroll + 2;
	y = gfx_text_wrap(PANE_X + 8, y, PANE_W - 16, 18.0f, 24, COLOR_TEXT, release->name);
	char when[48];
	snprintf(when, sizeof(when), "%s%s%s", release->date, release->date[0] && release->prerelease ? "    " : "",
	         release->prerelease ? "Pre-release" : "");
	if (when[0]) {
		gfx_text(PANE_X + 8, y, 13.0f, COLOR_ACCENT, 255, when);
		y += 24;
	}
	y += 6;
	if (!release->notes[0]) {
		gfx_text(PANE_X + 8, y, 14.0f, COLOR_DIM, 255, "This release has no notes.");
		y += 22;
	}
	// its notes, a line at a time: headings, the items of lists, and the rest
	static char line[1608]; // (as long as a line of notes can be)
	for (const char *at = release->notes; *at;) {
		const char *end = strchr(at, '\n');
		int len = end ? (int)(end - at) : (int)strlen(at);
		char mark = len && (at[0] == 1 || at[0] == 2) ? at[0] : 0;
		int from = mark ? 1 : 0, take = len - from < (int)sizeof(line) - 1 ? len - from : (int)sizeof(line) - 1;
		memcpy(line, at + from, (size_t)take);
		line[take] = '\0';
		at = end ? end + 1 : at + len;
		if (!take) {
			y += 9; // a gap
		} else if (mark == 1) {
			y = gfx_text_wrap(PANE_X + 8, y + 5, PANE_W - 16, 15.5f, 21, COLOR_ACCENT, line) + 3;
		} else if (mark == 2) {
			gfx_dot(PANE_X + 13, y + 10, 2, COLOR_ACCENT, 255);
			y = gfx_text_wrap(PANE_X + 26, y, PANE_W - 40, 14.0f, 19, COLOR_TEXT, line) + 3;
		} else {
			y = gfx_text_wrap(PANE_X + 8, y, PANE_W - 16, 14.0f, 19, COLOR_TEXT, line) + 3;
		}
	}
	rel_scroll_max = y + 8 + rel_scroll - PANE_Y - PANE_H;
	if (rel_scroll_max < 0)
		rel_scroll_max = 0;
	scroll_bar(rel_scroll, rel_scroll_max);
}

// Names on the left; on the right something that scrolls, over a row of
// buttons, which don't. During an install the right side shows its lines.
static void render_window(void) {
	const Project *p = &PROJECTS[win_project];
	static Release release; // (big, and only this thread draws)
	LiveInfo info;
	live_info(win_project, &info);
	if (win_photo >= info.photos)
		win_photo = 0;
	if (rel_index >= info.releases)
		rel_index = 0;
	bool has_release = win_level == LEVEL_RELEASES && live_release(win_project, rel_index, &release);
	if (chrome_kind == CHROME_WINDOW) {
		gfx_restore();
	} else {
		gfx_begin();
		gfx_font(FONT_TITLE);
		gfx_text_shadow(40, 22, 36.0f, COLOR_TEXT, "My other projects");
		gfx_font(FONT_TEXT);
		gfx_text_shadow(SCREEN_W - 40 - gfx_text_width(16.0f, VERSION_TEXT), 42, 16.0f, COLOR_DIM, VERSION_TEXT);
		gfx_panel(LIST_X, WIN_Y, LIST_W, WIN_H, 20, COLOR_PANEL, PANEL_ALPHA);
		gfx_panel(SIDE_X, WIN_Y, SIDE_W, WIN_H, 20, COLOR_PANEL, PANEL_ALPHA);
		gfx_panel(LEFT_X, 464, 896, 46, 14, COLOR_PANEL, PANEL_ALPHA);
		gfx_save();
		chrome_kind = CHROME_WINDOW;
	}

	// the left side
	if (win_level == LEVEL_PROJECTS) {
		for (int i = 0; i < PROJECT_COUNT; i++) {
			int y = WIN_Y + 16 + i * 68;
			bool on = i == win_project;
			if (on)
				gfx_panel(LIST_X + 12, y, LIST_W - 24, 58, 12, win_side ? RGB(255, 255, 255) : COLOR_ACCENT,
				          win_side ? 22 : 64);
			emblem(i, LIST_X + 42, y + 29, 17, 20.0f);
			gfx_text(LIST_X + 68, y + 8, 15.5f, on ? COLOR_TEXT : COLOR_DIM, 255, PROJECTS[i].name);
			gfx_text(LIST_X + 68, y + 31, 12.5f, on ? COLOR_ACCENT : COLOR_DIM, 255, PROJECTS[i].kind);
		}
	} else { // the project, and its releases under it
		static Release row; // (as above)
		emblem(win_project, LIST_X + 34, WIN_Y + 32, 17, 20.0f);
		gfx_text(LIST_X + 60, WIN_Y + 12, 15.5f, COLOR_TEXT, 255, p->name);
		gfx_text(LIST_X + 60, WIN_Y + 34, 12.5f, COLOR_ACCENT, 255, "Releases");
		// the chosen one is kept among the rows that show
		if (rel_index < rel_top)
			rel_top = rel_index;
		if (rel_index >= rel_top + RELEASE_ROWS)
			rel_top = rel_index - RELEASE_ROWS + 1;
		for (int k = 0; k < RELEASE_ROWS && live_release(win_project, rel_top + k, &row); k++) {
			int y = WIN_Y + 66 + k * 58;
			bool on = rel_top + k == rel_index;
			if (on)
				gfx_panel(LIST_X + 12, y, LIST_W - 24, 52, 12, win_side ? RGB(255, 255, 255) : COLOR_ACCENT,
				          win_side ? 22 : 64);
			fit_text(row.name, 14.5f, LIST_W - 24 - 28);
			gfx_text(LIST_X + 26, y + 6, 14.5f, on ? COLOR_TEXT : COLOR_DIM, 255, row.name);
			gfx_text(LIST_X + 26, y + 28, 12.5f, on ? COLOR_ACCENT : COLOR_DIM, 255,
			         row.prerelease ? "Pre-release" : row.date);
		}
		if (info.releases > RELEASE_ROWS) { // there are more than show
			char count[24];
			snprintf(count, sizeof(count), "%d of %d", rel_index + 1, info.releases);
			gfx_text(LIST_X + LIST_W - 16 - gfx_text_width(12.5f, count), WIN_Y + 34, 12.5f, COLOR_DIM, 255, count);
		}
	}

	// the right side: what scrolls, cut off at the edges of its space
	gfx_clip_rows(PANE_Y, PANE_Y + PANE_H);
	if (win_job) {
		char heading[64];
		snprintf(heading, sizeof(heading), job_running ? "Installing %s" : job_ok ? "Installed %s" : "%s wasn't installed",
		         p->name);
		gfx_text(PANE_X + 8, PANE_Y + 4, 16.0f, job_running ? COLOR_ACCENT : job_ok ? COLOR_GOOD : COLOR_BAD, 255, heading);
		int y = PANE_Y + 38;
		for (int i = 0; i < log_count && y < PANE_Y + PANE_H - 16; i++)
			y = gfx_text_wrap(PANE_X + 8, y, PANE_W - 16, 14.0f, 18, log_color[i], log_text[i]) + 4;
	} else if (win_level == LEVEL_PROJECTS) {
		pane_project(&info);
	} else {
		pane_release(&info, has_release ? &release : NULL);
	}
	gfx_clip_rows(0, SCREEN_H);

	// its buttons, left to right
	Button buttons[BUTTONS_MAX];
	int count = window_buttons(has_release ? &release : NULL, buttons), bx = PANE_X;
	int chosen = win_level == LEVEL_PROJECTS ? win_action : rel_action;
	for (int i = 0; i < count; i++) {
		const Button *b = &buttons[i];
		int w = gfx_text_width(15.0f, b->label) + (b->size[0] ? gfx_text_width(12.5f, b->size) + 10 : 0) + 32;
		bool on = win_side && i == chosen && !win_job;
		gfx_panel(bx, BUTTON_Y, w, BUTTON_H, 12, on ? COLOR_ACCENT : RGB(255, 255, 255), on ? 84 : 18);
		uint32_t color = win_job || (b->kind == ACT_DATA && !data_files && !on) ? COLOR_DIM : COLOR_TEXT;
		int tw = gfx_text(bx + 16, BUTTON_Y + 9, 15.0f, color, 255, b->label);
		if (b->size[0])
			gfx_text(bx + 16 + tw + 10, BUTTON_Y + 12, 12.5f, win_job ? COLOR_DIM : COLOR_ACCENT, 255, b->size);
		bx += w + 10;
	}

	char hint[160];
	const char *photos = win_level == LEVEL_PROJECTS && info.photos > 1 ? "L / R: photos      " : "";
	if (job_running)
		snprintf(hint, sizeof(hint), "One moment... Keep this app open.");
	else if (win_job)
		snprintf(hint, sizeof(hint), "%s%s: back", job_restart ? "START: restart the Vita now        " : "", back_name);
	else if (page_failed)
		snprintf(hint, sizeof(hint), "The browser wouldn't open. Its address is on the project's card.");
	else if (win_side)
		snprintf(hint, sizeof(hint), "Left / Right: choose      Up / Down: scroll      %s%s: select      %s: back", photos,
		         enter_name, back_name);
	else
		snprintf(hint, sizeof(hint), "Up / Down: choose      %s%s: its buttons      %s: back", photos, enter_name,
		         back_name);
	gfx_text(LEFT_X + 24, 476, 16.0f, COLOR_DIM, 255, hint);
	gfx_end();
}

// Engram: its own installer, through the system's installer. That app then
// puts the plugin in and keeps it up to date, so nothing of Engram's is
// placed from here.
static bool install_engram(const char *tag) {
	ProjectFile file;
	char title[64];
	if (!project_find(ENGRAM_REPO, tag, ENGRAM_VPK, &file, say))
		return false;
	uint8_t *vpk = project_fetch(&file, 16 * 1024 * 1024, say);
	if (!vpk)
		return false;
	bool ok = project_install_vpk(vpk, file.size, title, sizeof(title), say);
	free(vpk);
	if (!ok)
		return false;
	say(SAY_GOOD, false, "Done. Open %s from the home screen to install Engram.", title);
	return true;
}

// CyberFlow Launcher: its VPK through the system's installer and, with the
// switch on, its data files unpacked into ux0:data.
static bool install_cyberflow(const char *tag) {
	ProjectFile file;
	char title[64];
	if (!project_find(CYBERFLOW_REPO, tag, CYBERFLOW_VPK, &file, say))
		return false;
	uint8_t *vpk = project_fetch(&file, 40 * 1024 * 1024, say);
	if (!vpk)
		return false;
	bool ok = project_install_vpk(vpk, file.size, title, sizeof(title), say);
	free(vpk);
	if (!ok)
		return false;
	if (!data_files) {
		say(SAY_TEXT, false, "The data files were left out: the themes' videos and music.");
		return true;
	}
	if (!project_find(CYBERFLOW_REPO, tag, CYBERFLOW_DATA, &file, say) ||
	    !project_install_zip(&file, "RetroFlow", "ux0:data", say)) {
		say(SAY_TEXT, false, "The app is installed; its data files aren't. Install again to retry them.");
		return false;
	}
	say(SAY_GOOD, false, "Done.");
	return true;
}

// An install of a release, from the window. Everything it says goes up as it happens.
static void window_install(const char *tag) {
	log_clear();
	win_job = job_running = true;
	job_ok = job_restart = false;
	job_ok = win_project == PROJECT_ENGRAM ? install_engram(tag) : install_cyberflow(tag);
	job_running = false;
}

// ---- redrawing ----
// Redrawing the panels and text takes a few frames' worth of time, so it is
// done on a thread of its own, out of sight (gfx.h), while this one goes on
// drawing the liquid. When the new look is ready it is shown between two
// frames. While one is being drawn nothing it shows may change, so the
// buttons pressed meanwhile wait.
enum { REDRAW_IDLE, REDRAW_DRAWING, REDRAW_READY };
static volatile int redraw_state;
static SceUID redraw_wake = -1;
static struct {
	int view, selected;
	bool installed, ok;
	char perf[48];
} redraw_what;
static volatile int redraw_ms; // how long the last one took

static int redraw_thread(SceSize args, void *argp) {
	(void)args;
	(void)argp;
	for (;;) {
		sceKernelWaitSema(redraw_wake, 1, NULL);
		SceUInt64 before = sceKernelGetProcessTimeWide();
		if (redraw_what.view == VIEW_WINDOW)
			render_window();
		else
			render_ui(redraw_what.view, redraw_what.selected, redraw_what.installed, redraw_what.ok, redraw_what.perf);
		redraw_ms = (int)((sceKernelGetProcessTimeWide() - before) / 1000);
		redraw_state = REDRAW_READY;
	}
	return 0;
}

static bool redraw_start(void) {
	redraw_wake = sceKernelCreateSema("tweaks_redraw", 0, 0, 1, NULL);
	SceUID thread = sceKernelCreateThread("tweaks_redraw", redraw_thread, 0x10000100, 256 * 1024, 0, 0, NULL);
	return redraw_wake >= 0 && thread >= 0 && sceKernelStartThread(thread, 0, NULL) >= 0;
}

static unsigned buttons_pressed(unsigned *last) {
	SceCtrlData pad;
	memset(&pad, 0, sizeof(pad));
	sceCtrlPeekBufferPositive(0, &pad, 1);
	unsigned now = pad.buttons, pressed = now & ~*last;
	*last = now;
	return pressed;
}

int main(void) {
	scePowerSetArmClockFrequency(444); // the liquid is drawn on the CPU
	if (!gfx_init() || !liquid_init()) {
		sceKernelExitProcess(0);
		return 0;
	}
	int view = VIEW_MENU, selected = 0, working_frames = 0, frames = 0;
	bool ok = false, redraw = true, full[2] = {true, true}, perf_on = false, downloading = false;
	UpdateInfo update;
	char perf[48] = "";
	unsigned last = 0, queued = 0;
	int live_seen = -1;
	read_buttons();
	bool threaded = redraw_start(); // (without it, redraws happen here as they used to)
	live_start(LIVE, PROJECT_COUNT);
	updater = update_is_updater();
	if (updater) {
		// Opened by the installer to put its new version in. If that works
		// the new installer opens; if not, what went wrong stays on screen.
		busy_selected = selected = MENU_UPDATE;
		if (update_finish(say)) {
			sceKernelDelayThread(1500 * 1000);
			update_open_installer();
		}
		view = VIEW_RESULT;
		result_kind = RESULT_NOTE;
	} else {
		update_tidy();
	}
	SceUInt64 start = sceKernelGetProcessTimeWide(), spent = 0;
	for (;;) {
		// a look that has finished drawing goes on screen
		if (redraw_state == REDRAW_READY) {
			gfx_show();
			full[0] = full[1] = true;
			redraw_state = REDRAW_IDLE;
		}
		// buttons wait while one is being drawn
		queued |= buttons_pressed(&last);
		unsigned pressed = 0;
		bool idle = redraw_state == REDRAW_IDLE && !redraw;
		if (idle) {
			pressed = queued;
			queued = 0;
		}
		// what GitHub said, or a photo, has arrived
		if (view == VIEW_WINDOW && live_generation() != live_seen) {
			live_seen = live_generation();
			redraw = true;
		}
		if (pressed & SCE_CTRL_SELECT) { // how long a frame of liquid takes
			perf_on = !perf_on;
			perf[0] = '\0';
			frames = 0;
			spent = 0;
			redraw = true;
		}
		if (view == VIEW_MENU) {
			if (pressed & SCE_CTRL_START)
				break;
			if (pressed & SCE_CTRL_UP) {
				selected = (selected + MENU_COUNT - 1) % MENU_COUNT;
				redraw = true;
			}
			if (pressed & SCE_CTRL_DOWN) {
				selected = (selected + 1) % MENU_COUNT;
				redraw = true;
			}
			if (selected == MENU_PROJECTS && (pressed & (btn_enter | SCE_CTRL_RIGHT))) {
				view = VIEW_WINDOW;
				win_level = LEVEL_PROJECTS;
				win_project = win_action = win_scroll = win_photo = 0;
				win_side = win_job = page_failed = false;
				live_ask(); // the newest releases and photos, in the background
				redraw = true;
			} else if (pressed & btn_enter) {
				if (selected == MENU_EXIT)
					break;
				log_clear();
				view = VIEW_WORKING;
				working_frames = 0;
				redraw = true;
			}
		} else if (view == VIEW_WINDOW) {
			static Release chosen; // (big)
			Button buttons[BUTTONS_MAX];
			LiveInfo info;
			live_info(win_project, &info);
			bool releases = win_level == LEVEL_RELEASES;
			bool has_release = releases && live_release(win_project, rel_index, &chosen);
			int count = window_buttons(has_release ? &chosen : NULL, buttons);
			unsigned any = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE |
			               SCE_CTRL_TRIANGLE | SCE_CTRL_START | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
			if (pressed & any) {
				redraw = true;
				if (!win_job)
					page_failed = false;
			}
			int *action = releases ? &rel_action : &win_action, *scroll = releases ? &rel_scroll : &win_scroll;
			int scroll_max = releases ? rel_scroll_max : win_scroll_max;
			if (*action >= count)
				*action = 0;
			if (win_job) { // what an install said is up: read it, then back
				if ((pressed & SCE_CTRL_START) && job_restart)
					scePowerRequestColdReset();
				else if (pressed & (btn_enter | btn_back)) // either: there is only the one thing to do
					win_job = false;
			} else if (!releases && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
				if (info.photos > 0)
					win_photo = (win_photo + (pressed & SCE_CTRL_LTRIGGER ? info.photos - 1 : 1)) % info.photos;
			} else if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_START)) {
				view = VIEW_MENU;
			} else if (!win_side) { // the names
				if ((pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) && !releases) {
					win_project = (win_project + (pressed & SCE_CTRL_UP ? PROJECT_COUNT - 1 : 1)) % PROJECT_COUNT;
					win_action = win_scroll = win_photo = 0;
				} else if ((pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) && info.releases > 0) {
					rel_index = (rel_index + (pressed & SCE_CTRL_UP ? info.releases - 1 : 1)) % info.releases;
					rel_action = rel_scroll = 0;
				} else if ((pressed & (btn_enter | SCE_CTRL_RIGHT)) && count > 0) {
					win_side = true;
				} else if ((pressed & btn_back) && releases) {
					win_level = LEVEL_PROJECTS; // back to the project, on the button that led here
					win_side = true;
				} else if (pressed & btn_back) {
					view = VIEW_MENU;
				}
			} else if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) {
				*scroll += pressed & SCE_CTRL_UP ? -56 : 56;
				if (*scroll > scroll_max)
					*scroll = scroll_max;
				if (*scroll < 0)
					*scroll = 0;
			} else if (pressed & btn_back) {
				win_side = false; // back to the names
			} else if (pressed & SCE_CTRL_LEFT) {
				if (*action > 0)
					(*action)--;
				else
					win_side = false; // (the names are what is to the left of the first button)
			} else if (pressed & SCE_CTRL_RIGHT) {
				if (*action < count - 1)
					(*action)++;
			} else if ((pressed & btn_enter) && count > 0) {
				int kind = buttons[*action].kind;
				if (kind == ACT_DOWNLOAD) { // its releases
					win_level = LEVEL_RELEASES;
					win_side = false;
					rel_index = rel_top = rel_action = rel_scroll = 0;
				} else if (kind == ACT_DATA) {
					data_files = !data_files;
				} else if (kind == ACT_PAGE || kind == ACT_RELEASE_PAGE) {
					// the page, in the Vita's browser, over this app
					char uri[240];
					if (kind == ACT_PAGE)
						snprintf(uri, sizeof(uri), "webmodal: https://%s", PROJECTS[win_project].where);
					else
						snprintf(uri, sizeof(uri), "webmodal: https://%s/releases/tag/%s", PROJECTS[win_project].where,
						         chosen.tag);
					page_failed = sceAppMgrLaunchAppByUri(0x20000, uri) < 0;
				} else if (kind == ACT_INSTALL) {
					SceUInt64 before = sceKernelGetProcessTimeWide();
					busy_clock = (double)(before - start) / 1000000.0;
					window_install(chosen.tag);
					start += sceKernelGetProcessTimeWide() - before; // the liquid picks up where it was
				}
			}
		} else if (view == VIEW_WORKING) {
			// Both buffers have shown "Working..." by the third frame after it was drawn.
			if (idle && ++working_frames > 2) {
				SceUInt64 before = sceKernelGetProcessTimeWide();
				busy_selected = selected;
				busy_clock = (double)(before - start) / 1000000.0;
				if (downloading) {
					ok = update_install(&update, say);
					result_kind = ok ? RESULT_UPDATED : RESULT_NOTE;
				} else if (selected == MENU_UPDATE) {
					UpdateFound found = update_check(TWEAKS_VERSION, &update, say);
					ok = found != UPDATE_FAILED;
					result_kind = found == UPDATE_READY ? RESULT_UPDATE_READY : RESULT_NOTE;
				} else {
					ok = selected == MENU_INSTALL ? install() : uninstall();
					result_kind = RESULT_PLUGIN;
				}
				downloading = false;
				start += sceKernelGetProcessTimeWide() - before; // the liquid picks up where it was
				view = VIEW_RESULT;
				redraw = true;
			}
		} else {
			if ((pressed & SCE_CTRL_START) && result_kind == RESULT_PLUGIN) {
				scePowerRequestColdReset();
			} else if ((pressed & SCE_CTRL_START) && result_kind == RESULT_UPDATE_READY) {
				log_clear();
				downloading = true;
				view = VIEW_WORKING;
				working_frames = 0;
				redraw = true;
			} else if ((pressed & SCE_CTRL_START) && result_kind == RESULT_UPDATED) {
				update_open_updater(); // which ends this app
			} else if (pressed & (btn_enter | btn_back)) { // either: there is only the one thing to do
				if (updater)
					break;
				view = VIEW_MENU;
				redraw = true;
			}
		}
		if (redraw && redraw_state == REDRAW_IDLE) {
			redraw_what.view = view;
			redraw_what.selected = selected;
			redraw_what.installed = file_exists(PLUGIN_PATH);
			redraw_what.ok = ok;
			snprintf(redraw_what.perf, sizeof(redraw_what.perf), "%s", perf);
			redraw = false;
			if (threaded) {
				redraw_state = REDRAW_DRAWING;
				sceKernelSignalSema(redraw_wake, 1);
			} else {
				if (view == VIEW_WINDOW)
					render_window();
				else
					render_ui(view, selected, redraw_what.installed, ok, perf);
				gfx_show();
				full[0] = full[1] = true;
			}
		}
		SceUInt64 before = sceKernelGetProcessTimeWide();
		int buf = gfx_back_index();
		liquid_frame(gfx_back(), buf, (double)(before - start) / 1000000.0, full[buf], gfx_layers());
		full[buf] = false;
		spent += sceKernelGetProcessTimeWide() - before;
		if (perf_on && ++frames >= 30) {
			snprintf(perf, sizeof(perf), "liquid %u.%u ms    redraw %d ms", (unsigned)(spent / frames / 1000),
			         (unsigned)(spent / frames / 100 % 10), (int)redraw_ms);
			frames = 0;
			spent = 0;
			redraw = true;
		}
		gfx_present();
	}
	sceKernelExitProcess(0);
	return 0;
}
