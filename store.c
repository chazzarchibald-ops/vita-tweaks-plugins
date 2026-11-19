// Files under ur0:data/tweaks: the config, the log, and the page cache.

#include "tweaks.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <stdarg.h>

#define MAX_PLUGIN_LINES 64

static const char DEFAULT_CONFIG[] =
	"# Tweaks/Plugins " TWEAKS_VERSION "\n"
	"# enabled=0 turns the grouping off; Settings is back to normal the next time it opens.\n"
	"enabled=1\n"
	"# probe=0 stops the up-front check of the other Settings pages. Plugin pages are then\n"
	"# listed from the launch after you first open the page they are on.\n"
	"probe=1\n"
	"# log=0 stops log.txt being written. Also a switch at the top of Tweaks/Plugins.\n"
	"log=1\n"
	"#\n"
	"# log.txt lists every entry found, with its id. To fix one that's in the wrong place:\n"
	"#   move=<id>  put it in Tweaks/Plugins even though it has a custom widget or looks like one of Sony's\n"
	"#   keep=<id>  leave it where its plugin put it\n"
	"# To rename an entry in Tweaks/Plugins:\n"
	"#   name=<id>:<title>  for example name=id_yamt:YAMT (built in)\n"
	"# <id> is the id from log.txt, or the name you see: keep=yamt, move=Wi-Fi Turbo\n";

// Entries whose own titles don't say which plugin they belong to.
static const char *const DEFAULT_NAMES[][2] = {
	{"id_yamt", "YAMT"}, // "Storage Devices"
};

// ---- strings ----

int str_len(const char *s) {
	int n = 0;
	while (s && s[n])
		n++;
	return n;
}

int str_eq(const char *a, const char *b) {
	if (!a || !b)
		return 0;
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

int str_starts(const char *s, const char *prefix) {
	if (!s || !prefix)
		return 0;
	while (*prefix)
		if (*s++ != *prefix++)
			return 0;
	return 1;
}

int str_contains(const char *s, const char *needle) {
	for (; s && *s; s++)
		if (str_starts(s, needle))
			return 1;
	return 0;
}

static void str_copy(char *out, int out_size, const char *s, int n) {
	if (n > out_size - 1)
		n = out_size - 1;
	sceClibMemcpy(out, s, n);
	out[n] = '\0';
}

// ---- files ----

int file_exists(const char *path) {
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

void write_marker(const char *path) {
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0)
		sceIoClose(fd);
}

static int log_on = 1;

void log_enable(int on) {
	log_on = on;
}

// The log carries on from one launch to the next, reboots included, so a switch
// flipped before a reboot is still on record after it. Once it passes LOG_MAX it
// becomes OLD_LOG_PATH (replacing the one before) and a new one starts.
void log_rotate(void) {
	SceIoStat stat;
	if (sceIoGetstat(LOG_PATH, &stat) >= 0 && stat.st_size > LOG_MAX) {
		sceIoRemove(OLD_LOG_PATH);
		sceIoRename(LOG_PATH, OLD_LOG_PATH);
	}
}

void log_line(const char *fmt, ...) {
	if (!log_on)
		return;
	SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;

	char line[256];
	va_list args;
	va_start(args, fmt);
	sceClibVsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	sceIoWrite(fd, line, sceClibStrnlen(line, sizeof(line)));
	sceIoClose(fd);
}

// Reads up to max - 1 bytes and terminates them. Returns the length, or -1.
static int read_file(const char *path, char *buf, int max) {
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	int n = sceIoRead(fd, buf, max - 1);
	sceIoClose(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return n;
}

static int write_file(const char *path, const char *buf, int len) {
	SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;
	int n = sceIoWrite(fd, buf, len);
	sceIoClose(fd);
	return n;
}

// ---- crash guard ----

#define GUARD_RECORD 96 // GUARD_PATH is always this long, so a step is written in place

static SceUID guard_fd = -1;

int guard_check(char *last, int last_size) {
	static char record[GUARD_RECORD + 1];
	int n = read_file(GUARD_PATH, record, sizeof(record));
	if (n < 0)
		return 0;
	while (n > 0 && (record[n - 1] == ' ' || record[n - 1] == '\n' || record[n - 1] == '\r'))
		n--;
	if (n > 0)
		str_copy(last, last_size, record, n);
	else
		str_copy(last, last_size, "an unknown step", 15);
	return 1;
}

static void guard_write(const char *text) {
	char record[GUARD_RECORD];
	int n = str_len(text);
	sceClibMemset(record, ' ', sizeof(record));
	sceClibMemcpy(record, text, n < GUARD_RECORD - 1 ? n : GUARD_RECORD - 1);
	record[GUARD_RECORD - 1] = '\n';
	sceIoLseek(guard_fd, 0, SCE_SEEK_SET);
	sceIoWrite(guard_fd, record, sizeof(record));
}

void guard_arm(void) {
	// Written and closed once so its size is on disk, then kept open: a step
	// written just before a crash is then already in the file.
	guard_fd = sceIoOpen(GUARD_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (guard_fd < 0)
		return;
	guard_write("starting");
	sceIoClose(guard_fd);
	guard_fd = sceIoOpen(GUARD_PATH, SCE_O_WRONLY, 0666);
}

void guard_step(const char *fmt, ...) {
	if (guard_fd < 0)
		return;
	char text[GUARD_RECORD];
	va_list args;
	va_start(args, fmt);
	sceClibVsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	guard_write(text);
}

void guard_clear(void) {
	SceUID fd = guard_fd;
	guard_fd = -1;
	if (fd >= 0)
		sceIoClose(fd);
	sceIoRemove(GUARD_PATH);
	sceIoRemove(STRIKE_PATH);
}

// Calls fn on each line, cut at '\n' and trimmed of trailing '\r', spaces and tabs.
static void each_line(char *buf, int len, void (*fn)(char *line, void *ctx), void *ctx) {
	char *p = buf, *end = buf + len;
	while (p < end) {
		char *nl = p;
		while (nl < end && *nl != '\n')
			nl++;
		char *e = nl;
		while (e > p && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
			e--;
		*e = '\0';
		fn(p, ctx);
		p = nl + 1;
	}
}

// ---- config ----

static void set_name(Config *cfg, const char *id, int id_len, const char *title) {
	char key[TW_ID_LEN];
	str_copy(key, sizeof(key), id, id_len);
	int i = 0;
	while (i < cfg->names && !str_eq(cfg->name_id[i], key))
		i++;
	if (i == MAX_NAMES)
		return;
	if (i == cfg->names)
		cfg->names++;
	str_copy(cfg->name_id[i], TW_ID_LEN, key, str_len(key));
	str_copy(cfg->name_title[i], NAME_LEN, title, str_len(title));
}

const char *config_name(const Config *cfg, const char *id) {
	// Last first, so a name= line in the file beats a built-in one.
	for (int i = cfg->names - 1; i >= 0; i--)
		if (tw_name_matches(cfg->name_id[i], id, ""))
			return cfg->name_title[i];
	return NULL;
}

static void config_line(char *line, void *ctx) {
	Config *cfg = (Config *)ctx;
	if (line[0] == '#' || !line[0])
		return;
	if (str_starts(line, "enabled="))
		cfg->enabled = line[8] != '0';
	else if (str_starts(line, "probe="))
		cfg->probe = line[6] != '0';
	else if (str_starts(line, "log="))
		cfg->log = line[4] != '0';
	else if (str_starts(line, "move=") && line[5])
		tw_idset_add(&cfg->rules.move, line + 5);
	else if (str_starts(line, "keep=") && line[5])
		tw_idset_add(&cfg->rules.keep, line + 5);
	else if (str_starts(line, "name=")) {
		const char *id = line + 5, *colon = id;
		while (*colon && *colon != ':')
			colon++;
		// A title with a quote or angle bracket would break the XML it goes into.
		if (*colon && colon > id && colon[1] && !str_contains(colon + 1, "\"") && !str_contains(colon + 1, "<") &&
		    !str_contains(colon + 1, ">") && !str_contains(colon + 1, "&"))
			set_name(cfg, id, (int)(colon - id), colon + 1);
	}
}

void config_load(Config *cfg) {
	static char buf[4096];
	sceClibMemset(cfg, 0, sizeof(*cfg));
	cfg->enabled = 1;
	cfg->probe = 1;
	cfg->log = 1;
	for (unsigned i = 0; i < sizeof(DEFAULT_NAMES) / sizeof(*DEFAULT_NAMES); i++)
		set_name(cfg, DEFAULT_NAMES[i][0], str_len(DEFAULT_NAMES[i][0]), DEFAULT_NAMES[i][1]);

	int n = read_file(CONFIG_PATH, buf, sizeof(buf));
	if (n < 0) {
		write_file(CONFIG_PATH, DEFAULT_CONFIG, sizeof(DEFAULT_CONFIG) - 1);
		return;
	}
	each_line(buf, n, config_line, cfg);
}

// ---- which plugins are installed ----

// A cached entry is only shown while every Settings plugin that was installed
// when it was cached still is. Plugins are named by a hash of their lines under
// *NPXS10015 and *ALL in the taiHEN config, so adding a plugin keeps the cache
// and removing one drops entries cached since it was added.

typedef struct {
	int in_section;
	int count;
	unsigned int hash[MAX_PLUGIN_LINES];
} PluginLines;

static unsigned int fnv1a(const char *s) {
	unsigned int h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

static void plugin_line(char *line, void *ctx) {
	PluginLines *pl = (PluginLines *)ctx;
	while (*line == ' ' || *line == '\t')
		line++;
	if (line[0] == '#' || !line[0])
		return;
	if (line[0] == '*') {
		pl->in_section = str_eq(line, "*NPXS10015") || str_eq(line, "*ALL");
		return;
	}
	if (pl->in_section && pl->count < MAX_PLUGIN_LINES)
		pl->hash[pl->count++] = fnv1a(line);
}

static void plugin_lines(PluginLines *pl) {
	static char buf[8192];
	sceClibMemset(pl, 0, sizeof(*pl));
	// taiHEN reads ux0:tai/config.txt if it exists, else ur0:tai/config.txt.
	int n = read_file("ux0:tai/config.txt", buf, sizeof(buf));
	if (n < 0)
		n = read_file("ur0:tai/config.txt", buf, sizeof(buf));
	if (n >= 0)
		each_line(buf, n, plugin_line, pl);
}

static int has_hash(const PluginLines *pl, unsigned int h) {
	for (int i = 0; i < pl->count; i++)
		if (pl->hash[i] == h)
			return 1;
	return 0;
}

// ---- cache ----
//
// One file per page, CACHE_DIR/<page plugin>.xml:
//   <!-- tw-plugins 1a2b3c4d 5e6f7a8b ... -->
//   <!-- tw-id engram_settings -->
//   <setting_list id="engram_settings" ...>...</setting_list>
//   <!-- tw-id ... -->

static const char HEAD_OPEN[] = "<!-- tw-plugins";
static const char ID_OPEN[] = "\n<!-- tw-id ";
static const char COMMENT_CLOSE[] = " -->";

static const char *find(const char *p, const char *end, const char *s) {
	int n = str_len(s);
	for (; p + n <= end; p++)
		if (str_starts(p, s))
			return p;
	return 0;
}

static int hex_value(const char *p, const char *end, unsigned int *out) {
	unsigned int v = 0;
	int n = 0;
	for (; p < end; p++, n++) {
		char c = *p;
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
		if (d < 0)
			break;
		v = v * 16 + d;
	}
	*out = v;
	return n;
}

// Whether every plugin hash in the file's header is still in the taiHEN config.
static int header_current(const char *buf, const char *end, const PluginLines *now) {
	if (!str_starts(buf, HEAD_OPEN))
		return 0;
	const char *close = find(buf, end, COMMENT_CLOSE);
	if (!close)
		return 0;
	for (const char *p = buf + sizeof(HEAD_OPEN) - 1; p < close;) {
		if (*p == ' ') {
			p++;
			continue;
		}
		unsigned int h;
		int n = hex_value(p, close, &h);
		if (n == 0 || !has_hash(now, h))
			return 0;
		p += n;
	}
	return 1;
}

// Pages run from 1 KB to 50 (YAMT's), so cache files get a buffer of their own size.
typedef struct {
	SceUID uid;
	char *p;
} Block;

static int block_alloc(Block *b, int size) {
	b->p = NULL;
	b->uid = sceKernelAllocMemBlock("tweaks_cache", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (size + 0xFFF) & ~0xFFF, NULL);
	if (b->uid < 0) {
		log_line("no memory for a %d byte cache file: 0x%08X\n", size, b->uid);
		return b->uid;
	}
	sceKernelGetMemBlockBase(b->uid, (void **)&b->p);
	return 0;
}

static void block_free(Block *b) {
	if (b->uid >= 0)
		sceKernelFreeMemBlock(b->uid);
	b->uid = -1;
}

static int collect_file(const char *path, const PluginLines *now, cache_add_t add) {
	SceIoStat stat;
	Block file;
	int count = 0;
	if (sceIoGetstat(path, &stat) < 0 || stat.st_size <= 0 || block_alloc(&file, (int)stat.st_size + 1) < 0)
		return 0;
	int n = read_file(path, file.p, (int)stat.st_size + 1);
	const char *end = file.p + n;
	if (n <= 0 || !header_current(file.p, end, now)) {
		if (n > 0)
			log_line("cache %s: cached before a plugin was removed, left out\n", path);
		block_free(&file);
		return 0;
	}

	const char *p = find(file.p, end, ID_OPEN);
	while (p) {
		const char *id = p + sizeof(ID_OPEN) - 1;
		const char *id_end = find(id, end, COMMENT_CLOSE);
		if (!id_end)
			break;
		const char *block = id_end + sizeof(COMMENT_CLOSE) - 1;
		if (block < end && *block == '\n')
			block++;
		const char *next = find(block, end, ID_OPEN);
		const char *block_end = next ? next : end;

		char id_text[TW_ID_LEN];
		str_copy(id_text, sizeof(id_text), id, (int)(id_end - id));
		log_line("  cached: %s\n", id_text);
		add(id_text, block, (int)(block_end - block));
		count++;
		p = next;
	}
	block_free(&file);
	return count;
}

int cache_collect(cache_add_t add) {
	PluginLines now;
	plugin_lines(&now);

	SceUID dir = sceIoDopen(CACHE_DIR);
	if (dir < 0)
		return 0;
	int count = 0;
	SceIoDirent entry;
	while (sceIoDread(dir, &entry) > 0) {
		int len = str_len(entry.d_name);
		if (len < 5 || !str_eq(entry.d_name + len - 4, ".xml"))
			continue;
		char path[160];
		sceClibSnprintf(path, sizeof(path), CACHE_DIR "/%s", entry.d_name);
		count += collect_file(path, &now, add);
	}
	sceIoDclose(dir);
	return count;
}

void cache_store(const char *page, const char *xml, const tw_block_t *blocks, int nblocks) {
	char path[160];
	sceClibSnprintf(path, sizeof(path), CACHE_DIR "/%s.xml", page);

	if (nblocks == 0) {
		sceIoRemove(path);
		return;
	}

	PluginLines now;
	plugin_lines(&now);
	int size = sizeof(HEAD_OPEN) + now.count * 9 + sizeof(COMMENT_CLOSE);
	for (int i = 0; i < nblocks; i++)
		size += sizeof(ID_OPEN) + TW_ID_LEN + sizeof(COMMENT_CLOSE) + 1 + blocks[i].end - blocks[i].start;

	// Two halves of one buffer: the file as it should be, and as it is now.
	Block both;
	if (block_alloc(&both, size * 2 + 2) < 0)
		return;
	char *buf = both.p, *old = both.p + size;

	int pos = sceClibSnprintf(buf, size, "%s", HEAD_OPEN);
	for (int i = 0; i < now.count; i++)
		pos += sceClibSnprintf(buf + pos, size - pos, " %08x", now.hash[i]);
	pos += sceClibSnprintf(buf + pos, size - pos, "%s", COMMENT_CLOSE);
	for (int i = 0; i < nblocks; i++) {
		int len = blocks[i].end - blocks[i].start;
		pos += sceClibSnprintf(buf + pos, size - pos, "%s%s%s\n", ID_OPEN, blocks[i].id, COMMENT_CLOSE);
		sceClibMemcpy(buf + pos, xml + blocks[i].start, len);
		pos += len;
	}

	int old_len = read_file(path, old, size + 2);
	if (old_len != pos || sceClibMemcmp(old, buf, pos) != 0) {
		int ret = write_file(path, buf, pos);
		log_line("cache %s: %d entries, write 0x%08X\n", path, nblocks, ret);
	}
	block_free(&both);
}

// ---- switches we set ourselves ----
//
// DISABLED_PATH and NOPROBE_PATH hold the version that set them, so that
// installing another version gets a fresh try without anyone deleting a file.

void switch_set(const char *path) {
	write_file(path, TWEAKS_VERSION, sizeof(TWEAKS_VERSION) - 1);
}

int switch_on(const char *path) {
	char by[16];
	int n = read_file(path, by, sizeof(by));
	if (n < 0)
		return 0;
	if (str_eq(by, TWEAKS_VERSION))
		return 1;
	sceIoRemove(path);
	log_line("%s was set by %s; this is " TWEAKS_VERSION ", so trying again\n", path,
	         n > 0 ? by : "an older version");
	return 0;
}

// ---- changing our config from Settings ----

typedef struct {
	const char *key;        // drop lines that start with this, or if NULL...
	const char *id, *title; // ...the keep= and move= lines that name this entry
} Drop;

static int drops(const char *line, const Drop *d) {
	if (d->key)
		return str_starts(line, d->key);
	if (str_starts(line, "keep=") || str_starts(line, "move="))
		return tw_name_matches(line + 5, d->id, d->title);
	return 0;
}

// Rewrites CONFIG_PATH without the lines d picks, and with `add` as a new last line.
static void config_rewrite(const Drop *d, const char *add) {
	static char in[4096], out[4096 + 128];
	int n = read_file(CONFIG_PATH, in, sizeof(in));
	int pos = 0;
	for (char *p = in, *end = in + (n > 0 ? n : 0); p < end;) {
		char *nl = p;
		while (nl < end && *nl != '\n')
			nl++;
		char *e = nl;
		while (e > p && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
			e--;
		char saved = *e;
		*e = '\0';
		if (!drops(p, d)) {
			sceClibMemcpy(out + pos, p, e - p);
			pos += (int)(e - p);
			out[pos++] = '\n';
		}
		*e = saved;
		p = nl + 1;
	}
	if (add)
		pos += sceClibSnprintf(out + pos, sizeof(out) - pos, "%s\n", add);
	write_file(CONFIG_PATH, out, pos);
}

void config_set(const char *key, int value) {
	char prefix[24], line[32];
	sceClibSnprintf(prefix, sizeof(prefix), "%s=", key);
	sceClibSnprintf(line, sizeof(line), "%s=%d", key, value);
	Drop d = {prefix, NULL, NULL};
	config_rewrite(&d, line);
}

void config_set_shown(const char *id, const char *title, int shown, int custom) {
	char line[TW_ID_LEN + 8];
	Drop d = {NULL, id, title};
	// Shown is the default, except for an entry with a custom widget, which needs move=.
	sceClibSnprintf(line, sizeof(line), "%s=%s", shown ? "move" : "keep", id);
	config_rewrite(&d, !shown || custom ? line : NULL);
}

// ---- the taiHEN config ----
//
// Tweaks/Plugins can turn the plugin lines of the taiHEN config on and off: off
// puts a # straight in front of the line, on takes it away. Nothing else in the
// file is touched, and the lines for HENkaku and for Tweaks/Plugins itself are
// never shown or changed.

#define TAI_FILE_MAX 16384

static char tai_in[TAI_FILE_MAX], tai_out[TAI_FILE_MAX + 4];

static int same_ci(const char *a, const char *b, int n) {
	for (int i = 0; i < n; i++) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z')
			x += 32;
		if (y >= 'A' && y <= 'Z')
			y += 32;
		if (x != y)
			return 0;
	}
	return 1;
}

// The plugin path on the line [p, e), without the #s and spaces in front of it.
// Returns its length, or 0 if the line isn't a plugin; *off says it's commented out.
static int tai_plugin(const char *p, const char *e, const char **start, int *off) {
	*off = 0;
	while (p < e && (*p == '#' || *p == ' ' || *p == '\t')) {
		if (*p == '#')
			*off = 1;
		p++;
	}
	while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
		e--;
	int len = (int)(e - p), colon = 0;
	if (len < 8 || len >= TAI_PATH_LEN)
		return 0;
	for (int i = 0; i < len; i++) {
		if (p[i] == ' ' || p[i] == '\t')
			return 0;
		if (p[i] == ':')
			colon = 1;
	}
	if (!colon || (!same_ci(e - 6, ".suprx", 6) && !same_ci(e - 6, ".skprx", 6)))
		return 0;
	*start = p;
	return len;
}

static int tai_hidden(const char *path, int len) {
	static const char *const hidden[] = {"henkaku.suprx", "henkaku.skprx", "taihen.skprx", "tweaks.suprx"};
	const char *base = path;
	for (int i = 0; i < len; i++)
		if (path[i] == '/' || path[i] == ':')
			base = path + i + 1;
	int n = (int)(path + len - base);
	for (unsigned i = 0; i < sizeof(hidden) / sizeof(*hidden); i++)
		if (str_len(hidden[i]) == n && same_ci(base, hidden[i], n))
			return 1;
	return 0;
}

// Reads the config taiHEN is using into tai_in. Returns its length, or -1.
static int tai_read(char *file, int file_size) {
	// taiHEN reads ux0:tai/config.txt if it exists, else ur0:tai/config.txt.
	static const char *const paths[] = {"ux0:tai/config.txt", "ur0:tai/config.txt"};
	for (unsigned i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
		int n = read_file(paths[i], tai_in, sizeof(tai_in));
		if (n >= 0) {
			str_copy(file, file_size, paths[i], str_len(paths[i]));
			// A file that fills the buffer may be longer, and writing it back would cut it short.
			return n < TAI_FILE_MAX - 1 ? n : -1;
		}
	}
	return -1;
}

static int tai_section(TaiConfig *t, const char *name, int len) {
	char text[TAI_SECTION_LEN];
	str_copy(text, sizeof(text), name, len);
	for (int i = 0; i < t->nsections; i++)
		if (str_eq(t->section[i], text))
			return i;
	if (t->nsections == TAI_MAX_SECTIONS)
		return -1;
	str_copy(t->section[t->nsections], TAI_SECTION_LEN, text, str_len(text));
	return t->nsections++;
}

int tai_load(TaiConfig *t) {
	sceClibMemset(t, 0, sizeof(*t));
	int n = tai_read(t->file, sizeof(t->file));
	if (n < 0) {
		log_line("taiHEN config: can't be read, or is over %d bytes: not listed\n", TAI_FILE_MAX - 2);
		return -1;
	}
	int section = -1, number = 0;
	for (const char *p = tai_in, *end = tai_in + n; p < end; number++) {
		const char *nl = p, *s = p;
		while (nl < end && *nl != '\n')
			nl++;
		while (s < nl && (*s == ' ' || *s == '\t'))
			s++;
		if (s < nl && *s == '*') {
			const char *e = nl;
			while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
				e--;
			section = tai_section(t, s, (int)(e - s));
		} else if (section >= 0 && t->nlines < TAI_MAX_LINES) {
			const char *start;
			int off;
			int len = tai_plugin(p, nl, &start, &off);
			if (len && !tai_hidden(start, len)) {
				TaiLine *l = &t->line[t->nlines++];
				l->number = number;
				l->section = section;
				l->on = !off;
				str_copy(l->path, sizeof(l->path), start, len);
			}
		}
		p = nl + 1;
	}
	return t->nlines;
}

int tai_set(TaiConfig *t, int index, int on) {
	static int backed_up;
	char file[sizeof(t->file)];
	if (index < 0 || index >= t->nlines)
		return -1;
	TaiLine *l = &t->line[index];

	// The file as it is now: something else may have changed it since Settings opened.
	int n = tai_read(file, sizeof(file));
	const char *p = tai_in, *end = tai_in + n;
	if (n < 0 || !str_eq(file, t->file))
		return -1;
	for (int i = 0; i < l->number && p < end; i++) {
		while (p < end && *p != '\n')
			p++;
		p++;
	}
	const char *nl = p, *start = p;
	int off;
	while (nl < end && *nl != '\n')
		nl++;
	int len = p < end ? tai_plugin(p, nl, &start, &off) : 0;
	if (len != str_len(l->path) || sceClibMemcmp(start, l->path, len) != 0) {
		log_line("taiHEN config: line %d is no longer %s: left alone\n", l->number + 1, l->path);
		return -1;
	}
	if (!backed_up) {
		write_file(TAI_BACKUP_PATH, tai_in, n);
		backed_up = 1;
	}

	const char *rest = nl > p && nl[-1] == '\r' ? nl - 1 : nl; // keep the line's own ending
	int pos = (int)(p - tai_in);
	sceClibMemcpy(tai_out, tai_in, pos);
	if (!on)
		tai_out[pos++] = '#';
	sceClibMemcpy(tai_out + pos, l->path, len);
	pos += len;
	sceClibMemcpy(tai_out + pos, rest, end - rest);
	pos += (int)(end - rest);

	int ret = write_file(t->file, tai_out, pos);
	if (ret != pos) {
		write_file(t->file, tai_in, n); // put back what was there
		log_line("taiHEN config: writing %s failed (0x%08X)\n", t->file, ret);
		return -1;
	}
	l->on = on;
	log_line("taiHEN config: %s line %d is now %s%s\n", t->file, l->number + 1, on ? "" : "#", l->path);
	return 0;
}
