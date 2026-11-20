// Tweaks/Plugins: gathers the Settings pages other plugins add into one
// "Tweaks/Plugins" row straight under HENkaku Settings, the way a jailbroken
// iPhone lists its tweaks together in Settings.
//
// tweaks.suprx goes under *NPXS10015 in the taiHEN config (the Settings app):
//   main.c     the hooks, and which page gets which edit
//   regroup.c  the XML text edits themselves (no hooks or IO; can be tested on a PC)
//   store.c    config file, log, and the cache of entries found on other pages

#pragma once

#include "regroup.h"

#define TWEAKS_VERSION "1.1"

#define DATA_DIR "ur0:data/tweaks"
#define CONFIG_PATH DATA_DIR "/config.txt"
#define LOG_PATH DATA_DIR "/log.txt"
#define CACHE_DIR DATA_DIR "/cache"
#define OLD_LOG_PATH DATA_DIR "/log_old.txt" // the log before this one, once it passed LOG_MAX
#define LOG_MAX (64 * 1024)
#define GUARD_PATH DATA_DIR "/building"        // exists until a launch has proved stable; holds its last step
#define STRIKE_PATH DATA_DIR "/strike"         // the last launch ended badly; a second in a row turns us off
#define DISABLED_PATH DATA_DIR "/disabled"     // keeps Tweaks/Plugins off until deleted
#define NOPROBE_PATH DATA_DIR "/noprobe"       // the up-front page check crashed Settings once: skip it

#define MAX_NAMES 16
#define NAME_LEN 48

typedef struct {
	int enabled;
	int probe; // 0: never check the other pages up front (probe=0)
	int log;   // 0: write no log.txt (log=0)
	tw_rules_t rules;
	// name=<id>:<title> lines: what an entry is called in Tweaks/Plugins
	int names;
	char name_id[MAX_NAMES][TW_ID_LEN];
	char name_title[MAX_NAMES][NAME_LEN];
} Config;

// The title Tweaks/Plugins gives an entry, or NULL to keep its own.
const char *config_name(const Config *cfg, const char *id);

// Reads CONFIG_PATH, writing a commented default first if it's missing.
void config_load(Config *cfg);
// Changes made from the Settings pages: a key=0/1 line, and whether an entry is
// listed in Tweaks/Plugins (shown) or left where its plugin put it.
void config_set(const char *key, int value);
void config_set_shown(const char *id, const char *title, int shown, int custom);

// The plugin lines of the taiHEN config, for turning on and off. See store.c.
#define TAI_MAX_LINES 192
#define TAI_MAX_SECTIONS 48
#define TAI_PATH_LEN 96
#define TAI_SECTION_LEN 24
#define TAI_BACKUP_PATH DATA_DIR "/tai_config_backup.txt" // the file before this launch's first change

typedef struct {
	short number;  // which line of the file, from 0
	short section; // index into TaiConfig.section
	int on;        // not commented out
	char path[TAI_PATH_LEN];
} TaiLine;

typedef struct {
	char file[32]; // the config taiHEN is using
	int nsections, nlines;
	char section[TAI_MAX_SECTIONS][TAI_SECTION_LEN]; // "*KERNEL", "*main", ... each once
	TaiLine line[TAI_MAX_LINES];
} TaiConfig;

// Returns how many plugin lines there are to show, or -1 if the file can't be used.
int tai_load(TaiConfig *tai);
// Turns one line on or off in the file. Returns 0 if it was written.
int tai_set(TaiConfig *tai, int index, int on);

// The fallback for when probing can't find Sony's pages: entries that plugins
// put on other pages are remembered here the first time their page opens, and
// join Tweaks/Plugins from the next launch.
//
// Calls add for every cached entry. Entries cached while a since-removed plugin
// was installed are left out, so a plugin's page never outlives the plugin.
// Returns how many there were.
typedef void (*cache_add_t)(const char *id, const char *block, int len);
int cache_collect(cache_add_t add);
// Replaces what's cached for one page with these blocks of its XML.
void cache_store(const char *page, const char *xml, const tw_block_t *blocks, int nblocks);

// String helpers: there is no libc.
int str_len(const char *s);
int str_eq(const char *a, const char *b);
int str_starts(const char *s, const char *prefix);
int str_contains(const char *s, const char *needle);

int file_exists(const char *path);
void write_marker(const char *path);

// Crash guard. GUARD_PATH is there from module_start until the top menu has been
// on screen a few seconds, and holds the step the launch had reached. A launch
// that finds it knows the one before ended badly.
//
// If the last launch ended badly: copies the step it reached to last, and
// returns 1. Its log is the last one in LOG_PATH.
int guard_check(char *last, int last_size);
void guard_arm(void);
void guard_step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void guard_clear(void);
// DISABLED_PATH and NOPROBE_PATH: set by us, and in force until the file is deleted
// or another version is installed.
void switch_set(const char *path);
int switch_on(const char *path);

void log_enable(int on);
void log_rotate(void);
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
