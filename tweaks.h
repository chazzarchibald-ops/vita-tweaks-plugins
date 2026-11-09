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

#define TWEAKS_VERSION "1.0"

#define DATA_DIR "ur0:data/tweaks"
#define CONFIG_PATH DATA_DIR "/config.txt"
#define LOG_PATH DATA_DIR "/log.txt"
#define CACHE_DIR DATA_DIR "/cache"
#define GUARD_PATH DATA_DIR "/building"   // exists from building the top menu until it has shown for a few seconds
#define DISABLED_PATH DATA_DIR "/disabled" // keeps Tweaks/Plugins off until deleted

#define MAX_NAMES 16
#define NAME_LEN 48

typedef struct {
	int enabled;
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

void log_reset(void);
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
