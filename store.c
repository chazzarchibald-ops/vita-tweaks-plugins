// Files under ur0:data/tweaks: the config, the log, and the page cache.

#include "tweaks.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <stdarg.h>

#define CACHE_FILE_MAX 16384
#define MAX_PLUGIN_LINES 64

static const char DEFAULT_CONFIG[] =
	"# Tweaks/Plugins " TWEAKS_VERSION "\n"
	"# enabled=0 turns the grouping off; Settings is back to normal the next time it opens.\n"
	"enabled=1\n"
	"#\n"
	"# log.txt lists every entry found, with its id. To fix one that's in the wrong place:\n"
	"#   move=<id>  put it in Tweaks/Plugins even though it looks like one of Sony's\n"
	"#   keep=<id>  leave it where its plugin put it\n"
	"# To rename an entry in Tweaks/Plugins:\n"
	"#   name=<id>:<title>  for example name=id_yamt:YAMT (built in)\n";

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

void log_reset(void) {
	sceIoRemove(LOG_PATH);
}

void log_line(const char *fmt, ...) {
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
	for (int i = 0; i < cfg->names; i++)
		if (str_eq(cfg->name_id[i], id))
			return cfg->name_title[i];
	return NULL;
}

static void config_line(char *line, void *ctx) {
	Config *cfg = (Config *)ctx;
	if (line[0] == '#' || !line[0])
		return;
	if (str_starts(line, "enabled="))
		cfg->enabled = line[8] != '0';
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

static int collect_file(const char *path, const PluginLines *now, cache_add_t add) {
	static char buf[CACHE_FILE_MAX];
	int count = 0;
	int n = read_file(path, buf, sizeof(buf));
	if (n <= 0)
		return 0;
	const char *end = buf + n;
	if (!header_current(buf, end, now)) {
		log_line("cache %s: cached before a plugin was removed, left out\n", path);
		return 0;
	}

	const char *p = find(buf, end, ID_OPEN);
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
	static char buf[CACHE_FILE_MAX];
	static char old[CACHE_FILE_MAX];
	char path[160];
	sceClibSnprintf(path, sizeof(path), CACHE_DIR "/%s.xml", page);

	if (nblocks == 0) {
		sceIoRemove(path);
		return;
	}

	PluginLines now;
	plugin_lines(&now);
	int pos = sceClibSnprintf(buf, sizeof(buf), "%s", HEAD_OPEN);
	for (int i = 0; i < now.count && pos < (int)sizeof(buf); i++)
		pos += sceClibSnprintf(buf + pos, sizeof(buf) - pos, " %08x", now.hash[i]);
	pos += sceClibSnprintf(buf + pos, sizeof(buf) - pos, "%s", COMMENT_CLOSE);

	for (int i = 0; i < nblocks; i++) {
		int len = blocks[i].end - blocks[i].start;
		int head = str_len(ID_OPEN) + str_len(blocks[i].id) + str_len(COMMENT_CLOSE) + 1;
		if (pos + head + len >= (int)sizeof(buf)) {
			log_line("cache %s: no room for %s\n", path, blocks[i].id);
			break;
		}
		pos += sceClibSnprintf(buf + pos, sizeof(buf) - pos, "%s%s%s\n", ID_OPEN, blocks[i].id, COMMENT_CLOSE);
		sceClibMemcpy(buf + pos, xml + blocks[i].start, len);
		pos += len;
	}

	int old_len = read_file(path, old, sizeof(old));
	if (old_len == pos && sceClibMemcmp(old, buf, pos) == 0)
		return;
	int ret = write_file(path, buf, pos);
	log_line("cache %s: %d entries, write 0x%08X\n", path, nblocks, ret);
}
