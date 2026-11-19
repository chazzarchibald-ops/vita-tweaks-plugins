// The Settings app (NPXS10015) side of Tweaks/Plugins: the only side there is.
//
// Same recipe as Engram's settings.cpp and HENkaku: hook SceSettings'
// sceKernelLoadStartModule to wait for system_settings_core.suprx, and hook
//   - scePafMiscLoadXmlLayout, to edit each page's XML on its way to the parser:
//     the top menu gets the Tweaks/Plugins list, and other pages lose the
//     entries that list now holds;
//   - scePafToplevelGetText, for our own few strings.
//
// Unlike Engram we have to see each page after every other plugin has edited
// it (HENkaku swaps the whole top menu, Engram splices into System), so our XML
// hook must be the last in the chain. taiHEN puts each new hook straight after
// the first one in a chain, so we add two hooks back to back: every hook added
// later lands between them, and the one of ours whose next is NULL is last.
// That one does the work; the other passes straight through. The XML import is
// hooked when the core loads, as HENkaku does it, and the same trick on
// sceKernelLoadStartModule makes ours the first to see that happen.
//
// It only runs in the Settings app proper. NPXS10016, the Settings pop-up other
// apps open for one page, is the same program under another title id, and
// HENkaku's config has a section for it; there we do nothing.
//
// The top menu loads before any other page, so to know what plugins add to the
// other pages we probe them: when the top menu loads, find each page's stock XML
// in the Settings modules' memory, and send it through SceSettings' own import
// stub, so it runs the whole hook chain exactly as if the page were opening.
// Our hook is last, so it sees what every plugin added, takes notes, and returns
// without letting Paf parse it. If no stock XML turns up, the page cache in
// store.c is the fallback.

#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/registrymgr.h>
#include <psp2/rtc.h>
#include <psp2/vshbridge.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <taihen.h>

#include "tweaks.h"

// GCC emits calls to these even with -nostdlib; there is no libc to provide them.
void *memset(void *dst, int value, size_t size) {
	return sceClibMemset(dst, value, size);
}
void *memcpy(void *dst, const void *src, size_t size) {
	return sceClibMemcpy(dst, src, size);
}
size_t strlen(const char *s) {
	return sceClibStrnlen(s, 0x7FFFFFFF);
}

#define LIB_SCELIBKERNEL 0xCAE9ACE6
#define NID_LOAD_START_MODULE 0x2DCC4AFA
#define LIB_SCEPAFMISC 0x3D643CE8
#define NID_LOAD_XML_LAYOUT 0x19FE55A8
#define LIB_SCEPAFTOPLEVEL 0x4D9A9DD0
#define NID_GET_TEXT 0x19CEFDA7
#define LIB_SCEREGMGR 0xC436F916
#define NID_REG_GET_INT 0x16DDF3DC
#define NID_REG_SET_INT 0xD72EA399
#define NID_REG_GET_INFO 0x58421DD1
#define NID_INIT_PLUGIN_FUNCTIONS 0xF5354FEF

#define SETTINGS_TITLE_ID "NPXS10015"

#define ROOT_PLUGIN "system_settings_plugin" // the top menu
#define HENKAKU_PLUGIN "idu_settings_plugin" // HENkaku Settings' own page: never touched

#define LIST_START 0x10000   // the list being built grows from here
#define STOCK_MAX 192        // stock page XMLs we'll probe (3.65 has at least 48)
#define STOCK_PAGE_MAX 65536 // a "page" longer than this isn't one

typedef int (*LoadXmlLayout)(void *out, void *buf, int size, void *a4);

static Config cfg;
static tw_idset_t grouped; // ids in this launch's Tweaks/Plugins list
static tw_block_t blocks[TW_MAX_BLOCKS];
static tw_block_t probe_blocks[TW_MAX_BLOCKS];

// ---- memory ----
//
// Pages vary a lot in size (YAMT's Devices page is 50 KB), so buffers are memory
// blocks sized to each one. Paf may keep pointing into a page's XML after the load
// call returns (freeing the top menu's straight after crashed Settings), so an
// edited page's buffer is kept until that same page loads again.

typedef struct {
	SceUID uid;
	char *p;
	int size;
	char page[48]; // which page's XML it holds, when it holds one
} Mem;

static int mem_alloc(Mem *m, int size) {
	m->uid = -1;
	m->size = (size + 0xFFF) & ~0xFFF;
	SceUID uid = sceKernelAllocMemBlock("tweaks", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, m->size, NULL);
	if (uid < 0) {
		log_line("no memory for %d bytes: 0x%08X\n", m->size, uid);
		return uid;
	}
	m->uid = uid;
	sceKernelGetMemBlockBase(uid, (void **)&m->p);
	return 0;
}

static void mem_free(Mem *m) {
	if (m->uid >= 0)
		sceKernelFreeMemBlock(m->uid);
	m->uid = -1;
}

#define KEPT_MAX 8
static Mem kept[KEPT_MAX];
static int kept_next;

// Holds on to the XML just parsed for a page, freeing what that page had before.
static void keep_page(Mem *m) {
	int slot = -1;
	for (int i = 0; i < KEPT_MAX; i++)
		if (kept[i].uid >= 0 && str_eq(kept[i].page, m->page))
			slot = i;
	if (slot < 0) {
		for (int i = 0; i < KEPT_MAX && slot < 0; i++)
			if (kept[i].uid < 0)
				slot = i;
	}
	if (slot < 0) {
		slot = kept_next; // all in use: reuse the oldest
		kept_next = (kept_next + 1) % KEPT_MAX;
	}
	mem_free(&kept[slot]);
	kept[slot] = *m;
	m->uid = -1;
}

// ---- the list being built ----

static Mem list;
static int list_len;

static int list_reserve(int more) {
	if (list_len + more <= list.size)
		return 0;
	Mem bigger;
	int size = list.size * 2;
	while (size < list_len + more)
		size *= 2;
	if (mem_alloc(&bigger, size) < 0)
		return -1;
	sceClibMemcpy(bigger.p, list.p, list_len);
	mem_free(&list);
	list = bigger;
	return 0;
}

// ---- every plugin entry found ----
//
// Listed or left in place: each gets a switch under Tweaks/Plugins Settings.

typedef struct {
	char id[TW_ID_LEN];
	char title[TW_ID_LEN]; // its own title: a text id its plugin answers for, or plain text
	int custom;            // has a <user_custom>: only listed if move= says so
	int shown;             // listed in Tweaks/Plugins; the switch changes it for the next launch
} Found;

static Found found[TW_MAX_IDS];
static int nfound;

static void found_add(const char *id, const char *title, int custom, int shown) {
	for (int i = 0; i < nfound; i++)
		if (str_eq(found[i].id, id))
			return;
	if (nfound == TW_MAX_IDS)
		return;
	Found *f = &found[nfound++];
	sceClibSnprintf(f->id, sizeof(f->id), "%s", id);
	sceClibSnprintf(f->title, sizeof(f->title), "%s", title);
	f->custom = custom;
	f->shown = shown;
}

// Adds one plugin entry to Tweaks/Plugins, under its name= title if it has one.
static void add_entry(const char *id, const char *block, int len) {
	if (tw_idset_has(&grouped, id))
		return;
	const char *name = config_name(&cfg, id);
	int need = len + (name ? str_len(name) + 16 : 0) + 1;
	if (list_reserve(need) < 0) {
		log_line("  no room for %s\n", id);
		return;
	}
	int n;
	if (name) {
		n = tw_retitle(block, len, name, list.p + list_len, list.size - list_len);
		log_line("  %s shown as \"%s\"\n", id, name);
	} else {
		sceClibMemcpy(list.p + list_len, block, len);
		n = len;
	}
	if (n < 0)
		return;
	list_len += n;
	list.p[list_len++] = '\n';
	tw_idset_add(&grouped, id);
}

// ---- the XML edits ----

static void log_blocks(const tw_block_t *b, int n) {
	for (int i = 0; i < n; i++)
		log_line("  %s%s\n", b[i].id,
		         b[i].kept       ? " (left in place: switched off in Tweaks/Plugins Settings, or keep= in the config)"
		         : !b[i].movable ? " (has a custom widget, left in place: move= it to try anyway)"
		         : b[i].buttons  ? " (has buttons: if they stop working here, keep= it)"
		                         : "");
}

static void add_blocks(const char *xml, const tw_block_t *b, int n) {
	for (int i = 0; i < n; i++) {
		found_add(b[i].id, b[i].title, b[i].custom, b[i].movable);
		if (b[i].movable)
			add_entry(b[i].id, xml + b[i].start, b[i].end - b[i].start);
	}
}

// The same for an entry remembered from an earlier launch, which is just its XML.
static void add_cached(const char *id, const char *block, int len) {
	char title[TW_ID_LEN];
	tw_tag_attr(block, len, "title", title, sizeof(title));
	int custom = tw_has(block, len, "<user_custom");
	int shown = !tw_rule_has(&cfg.rules.keep, id, title) && (!custom || tw_rule_has(&cfg.rules.move, id, title));
	found_add(id, title, custom, shown);
	if (shown)
		add_entry(id, block, len);
}

// ---- probing the other pages ----

typedef struct {
	const char *xml;
	int size;
	char plugin[48];
} StockPage;

static StockPage stock[STOCK_MAX];
static tw_rules_t no_rules;
static int can_probe;        // our acting XML hook is the last in the chain
static const char *no_probe; // why probing is off, when it is
static struct {
	int active;
	const StockPage *page; // the page being probed
	int seen;              // whether the probe reached our hook
} probe;

static const char *mem_find(const char *p, const char *end, const char *s) {
	int n = str_len(s);
	for (; p + n <= end; p++)
		if (p[0] == s[0] && sceClibMemcmp(p, s, n) == 0)
			return p;
	return NULL;
}

// The stub SceSettings calls for an import: calling it runs every hook on it.
static void *import_stub(const char *module, uint32_t lib_nid, uint32_t func_nid) {
	tai_module_info_t info;
	info.size = sizeof(info);
	if (taiGetModuleInfo(module, &info) < 0)
		return NULL;
	for (uintptr_t p = info.imports_start; p < info.imports_end;) {
		uint16_t size = *(uint16_t *)p;
		uint16_t count = *(uint16_t *)(p + 6);
		uint32_t nid, *nids;
		void **stubs;
		if (size == 0x34) {
			nid = *(uint32_t *)(p + 0x10);
			nids = *(uint32_t **)(p + 0x1C);
			stubs = *(void ***)(p + 0x20);
		} else if (size == 0x24) {
			nid = *(uint32_t *)(p + 0x0C);
			nids = *(uint32_t **)(p + 0x14);
			stubs = *(void ***)(p + 0x18);
		} else {
			return NULL;
		}
		if (nid == lib_nid)
			for (int i = 0; i < count; i++)
				if (nids[i] == func_nid)
					return stubs[i];
		p += size;
	}
	return NULL;
}

// Finds every stock page XML in one segment: a <system_settings plugin="..."> up to
// its </system_settings>, plus the <?xml ...?> line and byte order mark before it,
// so plugins that check fixed offsets (as HENkaku does) see what they expect.
static int scan_segment(const char *seg, int seg_size, int count) {
	const char *end = seg + seg_size;
	for (const char *p = seg; count < STOCK_MAX && (p = mem_find(p, end, "<system_settings ")); p++) {
		const char *limit = end - p > STOCK_PAGE_MAX ? p + STOCK_PAGE_MAX : end;
		const char *close = mem_find(p, limit, "</system_settings>");
		if (!close)
			continue;
		StockPage *page = &stock[count];
		if (!tw_page_plugin(p, (int)(close - p), page->plugin, sizeof(page->plugin)))
			continue;

		const char *start = p;
		const char *back = p - seg > 64 ? p - 64 : seg;
		const char *decl = mem_find(back, p, "<?xml");
		if (decl) {
			start = decl;
			if (decl - seg >= 3 && sceClibMemcmp(decl - 3, "\xEF\xBB\xBF", 3) == 0)
				start = decl - 3;
		}
		const char *stop = close + sizeof("</system_settings>") - 1;
		while (stop < end && (*stop == '\r' || *stop == '\n'))
			stop++;

		page->xml = start;
		page->size = (int)(stop - start);
		count++;
		p = close;
	}
	return count;
}

// Stock page XMLs live in Settings' own modules, already decrypted in memory.
static int find_stock_pages(void) {
	static SceUID ids[128];
	static SceKernelModuleInfo info;
	SceSize num = sizeof(ids) / sizeof(*ids);
	int count = 0;
	if (sceKernelGetModuleList(0xFF, ids, &num) < 0)
		return 0;
	for (SceSize i = 0; i < num; i++) {
		sceClibMemset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelGetModuleInfo(ids[i], &info) < 0 || !str_contains(info.path, "NPXS10015"))
			continue;
		int before = count;
		for (int s = 0; s < 4; s++)
			if (info.segments[s].vaddr && info.segments[s].memsz)
				count = scan_segment((const char *)info.segments[s].vaddr, info.segments[s].memsz, count);
		if (count > before)
			log_line("%s: %d stock pages\n", info.module_name, count - before);
	}
	if (count == STOCK_MAX)
		log_line("found the most stock pages there's room for (%d): some may be left unprobed\n", STOCK_MAX);
	return count;
}

// Runs each stock page through the hook chain, collecting what plugins add.
// Returns how many pages were probed, or 0 if probing isn't possible.
static int probe_pages(void *out, void *a4) {
	if (no_probe)
		return 0;
	if (!can_probe) {
		// A probe has to stop at our hook. With another hook after ours it
		// would carry on into Paf and be parsed as if the page were opening.
		log_line("probe: skipped, another plugin's XML hook runs after ours\n");
		return 0;
	}
	guard_step("looking for Sony's pages in memory");
	LoadXmlLayout chain = (LoadXmlLayout)import_stub("SceSettings", LIB_SCEPAFMISC, NID_LOAD_XML_LAYOUT);
	int count = find_stock_pages();
	log_line("probe: chain stub %p, %d stock pages\n", chain, count);
	if (!chain || count == 0)
		return 0;

	int probed = 0;
	for (int i = 0; i < count; i++) {
		// HENkaku's own page is never looked at.
		if (str_eq(stock[i].plugin, HENKAKU_PLUGIN))
			continue;

		// The cache fallback tells Sony's entries from plugins' by name alone.
		// With Sony's pages in hand, note any it would get wrong on this firmware.
		int is_root = str_eq(stock[i].plugin, ROOT_PLUGIN);
		int odd = tw_find_blocks(stock[i].xml, stock[i].size, &no_rules, is_root ? tw_match_root : tw_match_page,
		                         NULL, probe_blocks, TW_MAX_BLOCKS);
		for (int j = 0; j < odd; j++)
			log_line("note: Sony's own %s on %s looks like a plugin's by name\n", probe_blocks[j].id,
			         stock[i].plugin);

		if (is_root)
			continue;
		guard_step("probing %s (%d of %d)", stock[i].plugin, i + 1, count);
		probe.active = 1;
		probe.page = &stock[i];
		probe.seen = 0;
		chain(out, (void *)stock[i].xml, stock[i].size, a4);
		probe.active = 0;
		if (probe.seen)
			probed++;
		else
			log_line("probe %s: never reached our hook\n", stock[i].plugin);
	}
	return probed;
}

// During a probe we have Sony's untouched page too, so an entry is a plugin's
// exactly when its id isn't in it, whatever the id looks like (YAMT's id_yamt).
static int match_not_stock(const char *id, const char *title, void *ctx) {
	const StockPage *page = (const StockPage *)ctx;
	char attr[TW_ID_LEN + 8];
	sceClibSnprintf(attr, sizeof(attr), "id=\"%s\"", id);
	return !mem_find(page->xml, page->xml + page->size, attr);
}

// Our last hook's side of a probe: note what the plugins added, parse nothing.
static void probe_capture(const char *xml, int size) {
	const StockPage *page = probe.page;
	probe.seen = 1;
	int n = tw_find_blocks(xml, size, &cfg.rules, match_not_stock, (void *)page, probe_blocks, TW_MAX_BLOCKS);
	if (n == 0)
		return;
	log_line("probe %s (%d bytes, stock %d), %d plugin entries:\n", page->plugin, size, page->size, n);
	log_blocks(probe_blocks, n);
	add_blocks(xml, probe_blocks, n);
}

// ---- our own rows ----
//
// At the top of Tweaks/Plugins, above the plugins:
//   Write Log                  a switch
//   Tweaks/Plugins Settings
//     Plugins in Settings      a switch per plugin entry found: listed here, or left in place
//     taiHEN Plugins           the taiHEN config's sections, and a switch per plugin line
// The switches are toggle_switch rows with keys under CATEGORY. That isn't in the
// registry: as Engram and HENkaku do, our hooks on the settings core's registry
// calls answer for it (see setting_get and setting_set).

#define CATEGORY "/CONFIG/TWEAKSPLUGINS"
#define REBOOT_BUTTON "tw_reboot" // see on_button

static TaiConfig tai;

// Appends formatted text, or sets *pos to -1 for good once it doesn't fit.
static void xml_put(Mem *m, int *pos, const char *fmt, ...) {
	if (*pos < 0)
		return;
	int room = m->size - *pos;
	va_list args;
	va_start(args, fmt);
	sceClibVsnprintf(m->p + *pos, room, fmt, args);
	va_end(args);
	int n = sceClibStrnlen(m->p + *pos, room);
	*pos = n >= room - 1 ? -1 : *pos + n;
}

// Text for an attribute value: the characters that would end or confuse it are swapped out.
static const char *attr_text(const char *text) {
	static char out[TAI_PATH_LEN];
	int i = 0;
	for (; text[i] && i < (int)sizeof(out) - 1; i++) {
		char c = text[i];
		out[i] = c == '"' ? '\'' : c == '<' ? '(' : c == '>' ? ')' : c == '&' ? '+' : c;
	}
	out[i] = '\0';
	return out;
}

// Returns the XML's length, or 0 (and no memory held) if it couldn't be built.
static int build_own_rows(Mem *rows) {
	if (mem_alloc(rows, 2048 + nfound * 256 + tai.nsections * 160 + tai.nlines * 256) < 0)
		return 0;
	int pos = 0;
	xml_put(rows, &pos,
	        "<toggle_switch id=\"tw_log\" title=\"tw_t_log\" icon=\"tex_spanner\" hint=\"tw_h_log\" key=\"" CATEGORY
	        "/log\"/>\n"
	        "<setting_list id=\"tw_settings\" title=\"tw_t_settings\" icon=\"tex_spanner\">\n"
	        "<setting_list id=\"tw_found\" title=\"tw_t_found\" icon=\"tex_spanner\">\n"
	        "<label id=\"tw_found_note\" title=\"tw_n_found_t\" description=\"tw_n_found\"/>\n");
	for (int i = 0; i < nfound; i++) {
		const char *name = config_name(&cfg, found[i].id);
		if (!name)
			name = found[i].title[0] ? found[i].title : found[i].id;
		xml_put(rows, &pos, "<toggle_switch id=\"tw_mv_%d\" title=\"%s\" icon=\"tex_spanner\" key=\"" CATEGORY "/mv_%d\"/>\n",
		        i, attr_text(name), i);
	}
	xml_put(rows, &pos, "</setting_list>\n");

	if (tai.nlines > 0) {
		xml_put(rows, &pos,
		        "<setting_list id=\"tw_tai\" title=\"tw_t_tai\" icon=\"tex_spanner\">\n"
		        "<button id=\"" REBOOT_BUTTON "\" title=\"tw_t_reboot\" icon=\"tex_spanner\"/>\n"
		        "<label id=\"tw_tai_note\" title=\"%s\" description=\"tw_n_tai\"/>\n",
		        attr_text(tai.file));
		for (int s = 0; s < tai.nsections; s++) {
			int open = 0;
			for (int i = 0; i < tai.nlines; i++) {
				if (tai.line[i].section != s)
					continue;
				if (!open++)
					xml_put(rows, &pos, "<setting_list id=\"tw_tai_s%d\" title=\"%s\" icon=\"tex_spanner\">\n", s,
					        attr_text(tai.section[s]));
				xml_put(rows, &pos, "<toggle_switch id=\"tw_tai_%d\" title=\"%s\" key=\"" CATEGORY "/tai_%d\"/>\n", i,
				        attr_text(tai.line[i].path), i);
			}
			if (open)
				xml_put(rows, &pos, "</setting_list>\n");
		}
		xml_put(rows, &pos, "</setting_list>\n");
	}
	xml_put(rows, &pos, "</setting_list>\n");
	if (pos < 0) {
		log_line("no room to build the Tweaks/Plugins Settings rows\n");
		mem_free(rows);
		return 0;
	}
	return pos;
}

// "mv_12" with prefix "mv_" is 12; -1 if name isn't that or the number isn't below count.
static int key_index(const char *name, const char *prefix, int count) {
	if (!str_starts(name, prefix))
		return -1;
	const char *p = name + str_len(prefix);
	int v = 0;
	if (!*p)
		return -1;
	for (; *p; p++) {
		if (*p < '0' || *p > '9' || v > 9999)
			return -1;
		v = v * 10 + (*p - '0');
	}
	return v < count ? v : -1;
}

static int setting_get(const char *name) {
	int i;
	if (str_eq(name, "log"))
		return cfg.log;
	if ((i = key_index(name, "mv_", nfound)) >= 0)
		return found[i].shown;
	if ((i = key_index(name, "tai_", tai.nlines)) >= 0)
		return tai.line[i].on;
	return 0;
}

static void setting_set(const char *name, int value) {
	int i;
	value = value != 0;
	if (str_eq(name, "log")) {
		cfg.log = value;
		config_set("log", value);
		log_enable(value);
		log_line("log turned on in Settings\n");
	} else if ((i = key_index(name, "mv_", nfound)) >= 0) {
		// Takes effect when the top menu is next built: the next time Settings opens.
		found[i].shown = value;
		config_set_shown(found[i].id, found[i].title, value, found[i].custom);
		log_line("%s: %s from the next launch\n", found[i].id, value ? "listed in Tweaks/Plugins" : "left in place");
	} else if ((i = key_index(name, "tai_", tai.nlines)) >= 0) {
		tai_set(&tai, i, value);
	}
}

// ---- the pages ----

// The edited XML goes in *done, which the caller frees once Paf has parsed it.
static void edit_root(void *out, void **buf, int *size, void *a4, Mem *done) {
	const char *xml = (const char *)*buf;
	guard_step("reading the top menu");
	int n = tw_find_blocks(xml, *size, &cfg.rules, tw_match_root, NULL, blocks, TW_MAX_BLOCKS);
	log_line("top menu (%d bytes), %d plugin entries:\n", *size, n);
	log_blocks(blocks, n);

	grouped.count = 0;
	nfound = 0;
	list_len = 0;
	if (mem_alloc(&list, LIST_START) < 0)
		return;
	add_blocks(xml, blocks, n);

	int probed = probe_pages(out, a4);
	if (probed > 0) {
		log_line("probed %d pages\n", probed);
	} else {
		log_line("no pages probed: using the ones remembered from earlier launches\n");
		cache_collect(add_cached);
	}

	guard_step("reading the taiHEN config");
	tai_load(&tai);
	log_line("taiHEN config %s: %d plugin lines in %d sections\n", tai.file, tai.nlines, tai.nsections);

	guard_step("building the new top menu");
	Mem rows;
	int rows_len = build_own_rows(&rows);
	int len = -1;
	if (mem_alloc(done, *size + rows_len + list_len + 1024) >= 0)
		len = tw_build_root(xml, *size, blocks, n, rows_len ? rows.p : NULL, rows_len, list.p, list_len, done->p,
		                    done->size);
	if (rows_len)
		mem_free(&rows);
	mem_free(&list);
	log_line("Tweaks/Plugins holds %d entries; top menu now %d bytes\n", grouped.count, len);
	if (len < 0) {
		mem_free(done);
		grouped.count = 0; // nothing was moved, so other pages must keep theirs
		return;
	}
	*buf = done->p;
	*size = len;
	guard_step("Settings is reading the new top menu");
}

// ---- crash guard ----
//
// The guard file (see tweaks.h) is there from module_start until the top menu has
// been on screen for a few seconds. A launch that finds it knows the last one
// ended badly, and two of those in a row turn Tweaks/Plugins off, so a bad edit
// can't lock anyone out of Settings. One alone doesn't: closing Settings within
// seconds of opening it looks the same from here.

#define GUARD_STABLE_MS 5000
#define GUARD_GIVE_UP_MS 30000 // the top menu never loaded: nothing of ours to guard

static volatile int top_menu_parsed;
static volatile int button_hooked; // see init_plugin_patched

static int guard_thread(SceSize args, void *argp) {
	int stable = 0;
	for (int ms = 0; ms < GUARD_GIVE_UP_MS && stable < GUARD_STABLE_MS; ms += 250) {
		sceKernelDelayThread(250 * 1000);
		if (top_menu_parsed)
			stable += 250;
	}
	guard_clear();
	log_line(top_menu_parsed ? "top menu stable for 5 s\n" : "the top menu never loaded: nothing was changed\n");
	if (top_menu_parsed && !button_hooked)
		log_line("note: the top menu's button handler hasn't been seen, so Reboot won't respond\n");
	return sceKernelExitDeleteThread(0);
}

// After Paf has parsed a page we looked at, edited or not.
static void parsed(Mem *done, int ret) {
	if (str_eq(done->page, ROOT_PLUGIN) && !top_menu_parsed) {
		log_line("top menu parsed: 0x%08X\n", ret);
		guard_step("showing the top menu");
		top_menu_parsed = 1;
	}
	if (done->uid >= 0)
		keep_page(done);
}

static void edit_page(const char *page, void **buf, int *size, Mem *done) {
	const char *xml = (const char *)*buf;

	// The fallback cache only has names to go by. Entries left in place are
	// remembered too, so they keep their switch under Tweaks/Plugins Settings.
	int n = tw_find_blocks(xml, *size, &cfg.rules, tw_match_page, NULL, blocks, TW_MAX_BLOCKS);
	cache_store(page, xml, blocks, n);

	// Take out exactly what this launch's list already shows.
	if (grouped.count == 0)
		return;
	n = tw_find_blocks(xml, *size, &cfg.rules, tw_match_ids, &grouped, blocks, TW_MAX_BLOCKS);
	if (n == 0 || mem_alloc(done, *size) < 0)
		return;
	int len = tw_strip_blocks(xml, *size, blocks, n, done->p, done->size);
	if (len < 0) {
		mem_free(done);
		return;
	}
	log_line("page %s: %d entries moved out\n", page, n);
	*buf = done->p;
	*size = len;
}

static void edit_xml(void *out, void **buf, int *size, void *a4, Mem *done) {
	char page[64];
	if (!cfg.enabled || !*buf || *size <= 0 || !tw_page_plugin((const char *)*buf, *size, page, sizeof(page)))
		return;
	if (str_eq(page, HENKAKU_PLUGIN) || str_contains(page, "/") || str_contains(page, ":"))
		return;
	sceClibSnprintf(done->page, sizeof(done->page), "%s", page);
	if (str_eq(page, ROOT_PLUGIN))
		edit_root(out, buf, size, a4, done);
	else
		edit_page(page, buf, size, done);
}

// ---- text ----

typedef unsigned short u16;
static u16 text_ring[8][256];
static int text_next;

static const u16 *to_u16(const char *text) {
	u16 *out = text_ring[text_next];
	text_next = (text_next + 1) % 8;
	int i = 0;
	for (; text[i] && i < 255; i++)
		out[i] = (unsigned char)text[i];
	out[i] = 0;
	return out;
}

static const char *text_for(const char *id) {
	if (!str_starts(id, "tw_"))
		return NULL;
	if (str_eq(id, "tw_title"))
		return "Tweaks/Plugins";
	if (str_eq(id, "tw_empty_title"))
		return "No plugin settings yet";
	if (str_eq(id, "tw_empty_text"))
		return "Plugins that add a page to Settings are listed here.";
	if (str_eq(id, "tw_about_title"))
		return "Version " TWEAKS_VERSION;
	if (str_eq(id, "tw_about_text"))
		return "Tweaks/Plugins by badmanwazzy37";
	if (str_eq(id, "tw_t_log"))
		return "Write Log";
	if (str_eq(id, "tw_h_log"))
		return "Records what Tweaks/Plugins finds each time Settings opens, in ur0:data/tweaks/log.txt. "
		       "Leave it on if you want to report a problem.";
	if (str_eq(id, "tw_t_settings"))
		return "Tweaks/Plugins Settings";
	if (str_eq(id, "tw_t_found"))
		return "Plugins in Settings";
	if (str_eq(id, "tw_n_found_t"))
		return "Show in Tweaks/Plugins";
	if (str_eq(id, "tw_n_found"))
		return "On: the plugin's settings are listed in Tweaks/Plugins. Off: they stay where the plugin put them. "
		       "Changes apply the next time Settings opens.";
	if (str_eq(id, "tw_t_tai"))
		return "taiHEN Plugins";
	if (str_eq(id, "tw_t_reboot"))
		return "Reboot";
	if (str_eq(id, "tw_n_tai"))
		return "Off puts a # in front of the plugin's line in this file, on takes it away. Kernel plugins change "
		       "after a reboot. Turning off a storage plugin such as YAMT can leave your memory card unavailable.";
	return NULL;
}

// ---- hooks ----

static SceUID load_hook[2] = {-1, -1}, xml_hook[2] = {-1, -1}, text_hook = -1;
static SceUID get_hook = -1, set_hook = -1, info_hook = -1, init_hook = -1;
static tai_hook_ref_t load_ref[2], xml_ref[2], text_ref, get_ref, set_ref, info_ref, init_ref;
static int core_hooked, warned_order;

static int is_last(SceUID uid, tai_hook_ref_t ref) {
	return uid >= 0 && ((struct _tai_hook_user *)ref)->next == 0;
}

// Whether XML hook `which` is the later of our two in the chain: the one that acts.
static int acts(int which) {
	if (xml_hook[which] < 0)
		return 0;
	if (xml_hook[which ^ 1] < 0)
		return 1;
	struct _tai_hook_user *other = (struct _tai_hook_user *)xml_ref[which ^ 1];
	struct _tai_hook_user *h = (struct _tai_hook_user *)xml_ref[which];
	for (int i = 0; i < 64 && h->next; i++) {
		h = (struct _tai_hook_user *)h->next;
		if (h == other)
			return 0;
	}
	return 1;
}

// Returns 1 when this call was a probe and must not reach Paf.
static int on_load_xml(int which, void *out, void **buf, int *size, void *a4, Mem *done) {
	done->uid = -1;
	done->page[0] = '\0';
	if (!acts(which))
		return 0;

	// Normally every other plugin's hook sits between our two, and this one is
	// last. If one got in after it, we still see and edit each page as it
	// opens, only without knowing what's on the others in advance.
	can_probe = is_last(xml_hook[which], xml_ref[which]);
	if (probe.active && can_probe) {
		probe_capture((const char *)*buf, *size);
		return 1;
	}
	if (!can_probe && !warned_order) {
		warned_order = 1;
		log_line("another plugin's XML hook runs after ours. Plugin pages can't be found up front: each one is "
		         "listed from the launch after you first open the page it's on. Putting tweaks.suprx first "
		         "under *" SETTINGS_TITLE_ID " in the taiHEN config may fix this.\n");
	}
	edit_xml(out, buf, size, a4, done);
	return 0;
}

// scePafMiscLoadXmlLayout(out, buf, size, a4)
static int load_xml_a_patched(void *out, void *buf, int size, void *a4) {
	Mem done;
	if (on_load_xml(0, out, &buf, &size, a4, &done))
		return 0;
	int ret = TAI_CONTINUE(int, xml_ref[0], out, buf, size, a4);
	parsed(&done, ret);
	return ret;
}

static int load_xml_b_patched(void *out, void *buf, int size, void *a4) {
	Mem done;
	if (on_load_xml(1, out, &buf, &size, a4, &done))
		return 0;
	int ret = TAI_CONTINUE(int, xml_ref[1], out, buf, size, a4);
	parsed(&done, ret);
	return ret;
}

static void hook_xml(void) {
	if (xml_hook[0] < 0)
		xml_hook[0] = taiHookFunctionImport(&xml_ref[0], "SceSettings", LIB_SCEPAFMISC, NID_LOAD_XML_LAYOUT,
		                                    load_xml_a_patched);
	if (xml_hook[1] < 0)
		xml_hook[1] = taiHookFunctionImport(&xml_ref[1], "SceSettings", LIB_SCEPAFMISC, NID_LOAD_XML_LAYOUT,
		                                    load_xml_b_patched);
}

// The settings core's registry calls, for the keys of our switches. As the core
// imports them: sceRegMgrGetKeyInt(category, name, &value), sceRegMgrSetKeyInt(
// category, name, value), sceRegMgrGetKeysInfo(category, info, unk).
static int is_ours(const char *category) {
	return category && str_eq(category, CATEGORY);
}

static int reg_get_int_patched(const char *category, const char *name, int *value) {
	if (is_ours(category) && name && value) {
		*value = setting_get(name);
		return 0;
	}
	return TAI_CONTINUE(int, get_ref, category, name, value);
}

static int reg_set_int_patched(const char *category, const char *name, int value) {
	if (is_ours(category) && name) {
		setting_set(name, value);
		return 0;
	}
	return TAI_CONTINUE(int, set_ref, category, name, value);
}

// Buttons. A press goes to the button handler of the page's plugin, the seventh of
// the callbacks scePafToplevelInitPluginFunctions(a1, a2, funcs) hands it; a1's
// second word is the plugin's name. Our rows are part of the top menu, so its
// handler is the one to stand in front of, as HENkaku does for its own page.
static int (*top_menu_button)(const char *id, int a2, void *a3);

static int on_button(const char *id, int a2, void *a3) {
	if (id && str_eq(id, REBOOT_BUTTON)) {
		// Settings is about to end on purpose. Left armed, a Reboot pressed within
		// seconds of opening Settings would count as a launch that ended badly.
		guard_clear();
		log_line("Reboot pressed\n");
		return scePowerRequestColdReset();
	}
	return top_menu_button ? top_menu_button(id, a2, a3) : 0;
}

static int init_plugin_patched(void *a1, int a2, uint32_t *funcs) {
	int ret = TAI_CONTINUE(int, init_ref, a1, a2, funcs);
	const char *plugin = a1 ? (const char *)((uint32_t *)a1)[1] : NULL;
	if (plugin && funcs && str_eq(plugin, ROOT_PLUGIN) && funcs[6] != (uint32_t)on_button) {
		top_menu_button = (int (*)(const char *, int, void *))funcs[6];
		funcs[6] = (uint32_t)on_button;
		button_hooked = 1;
		log_line("top menu's button handler hooked\n");
	}
	return ret;
}

static int reg_get_info_patched(const char *category, void *info, int unk) {
	if (is_ours(category)) {
		if (info)
			*(int *)((char *)info + 8) = 0x00040000; // an int key
		return 0;
	}
	return TAI_CONTINUE(int, info_ref, category, info, unk);
}

// scePafToplevelGetText(arg, char **id): the UTF-16 text for an id.
static void *get_text_patched(void *arg, char **id) {
	if (id && *id) {
		const char *text = text_for(*id);
		if (text)
			return (void *)to_u16(text);
	}
	return TAI_CONTINUE(void *, text_ref, arg, id);
}

static void core_loaded(SceUID ret, const char *path) {
	if (core_hooked || ret < 0 || !path || !str_contains(path, "system_settings_core"))
		return;
	core_hooked = 1;
	guard_step("hooking the settings core");
	hook_xml();
	text_hook = taiHookFunctionImport(&text_ref, "SceSystemSettingsCore", LIB_SCEPAFTOPLEVEL, NID_GET_TEXT,
	                                  get_text_patched);
	get_hook = taiHookFunctionImport(&get_ref, "SceSystemSettingsCore", LIB_SCEREGMGR, NID_REG_GET_INT,
	                                 reg_get_int_patched);
	set_hook = taiHookFunctionImport(&set_ref, "SceSystemSettingsCore", LIB_SCEREGMGR, NID_REG_SET_INT,
	                                 reg_set_int_patched);
	info_hook = taiHookFunctionImport(&info_ref, "SceSystemSettingsCore", LIB_SCEREGMGR, NID_REG_GET_INFO,
	                                  reg_get_info_patched);
	init_hook = taiHookFunctionImport(&init_ref, "SceSettings", LIB_SCEPAFTOPLEVEL, NID_INIT_PLUGIN_FUNCTIONS,
	                                  init_plugin_patched);

	tai_module_info_t core;
	core.size = sizeof(core);
	core.module_nid = 0;
	taiGetModuleInfo("SceSystemSettingsCore", &core);
	log_line("settings core loaded (nid 0x%08X): xml=0x%08X,0x%08X text=0x%08X reg=0x%08X,0x%08X,0x%08X\n",
	         (unsigned)core.module_nid, xml_hook[0], xml_hook[1], text_hook, get_hook, set_hook, info_hook);
	guard_step("waiting for the top menu");
}

// As SceSettings imports it, this takes six arguments, the last an int *status.
static SceUID load_start_a_patched(const char *path, SceSize args, void *argp, int flags, void *option,
                                   int *status) {
	SceUID ret = TAI_CONTINUE(SceUID, load_ref[0], path, args, argp, flags, option, status);
	core_loaded(ret, path);
	return ret;
}

static SceUID load_start_b_patched(const char *path, SceSize args, void *argp, int flags, void *option,
                                   int *status) {
	SceUID ret = TAI_CONTINUE(SceUID, load_ref[1], path, args, argp, flags, option, status);
	core_loaded(ret, path);
	return ret;
}

static void release(SceUID *uid, tai_hook_ref_t ref) {
	if (*uid >= 0)
		taiHookRelease(*uid, ref);
	*uid = -1;
}

// What a bug report needs: the firmware, and the things that differ by console.
// HENkaku's version spoofing changes what sceKernelGetSystemSwVersion says, so
// the real version comes from _vshSblGetSystemSwVersion, as VITAident and
// PSVident read it. The Settings module's own id is logged too: it's different
// for every firmware build.
static void log_system(const tai_module_info_t *settings) {
	SceKernelSystemSwVersion real, shown;
	sceClibMemset(&real, 0, sizeof(real));
	sceClibMemset(&shown, 0, sizeof(shown));
	real.size = shown.size = sizeof(real);
	int ret = _vshSblGetSystemSwVersion(&real);
	sceKernelGetSystemSwVersion(&shown);
	if (ret < 0)
		log_line("firmware unknown (0x%08X), shown as %s (0x%08X)\n", ret, shown.versionString,
		         (unsigned)shown.version);
	else if (real.version != shown.version)
		log_line("firmware %s (0x%08X), spoofed to %s (0x%08X)\n", real.versionString, (unsigned)real.version,
		         shown.versionString, (unsigned)shown.version);
	else
		log_line("firmware %s (0x%08X), not spoofed\n", real.versionString, (unsigned)real.version);

	int language = -1, enter = -1;
	sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "language", &language);
	sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "button_assign", &enter);
	log_line("SceSettings nid 0x%08X, language %d, enter button %s\n", (unsigned)settings->module_nid, language,
	         enter == 0 ? "circle" : enter == 1 ? "cross" : "unknown");
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
	tai_module_info_t info;
	info.size = sizeof(info);
	if (taiGetModuleInfo("SceSettings", &info) < 0)
		return SCE_KERNEL_START_SUCCESS; // listed for some other app by mistake: do nothing

	sceIoMkdir(DATA_DIR, 0777);
	sceIoMkdir(CACHE_DIR, 0777);

	char title_id[16] = "";
	int ret = sceAppMgrAppParamGetString(0, 12, title_id, sizeof(title_id));
	if (ret >= 0 && !str_eq(title_id, SETTINGS_TITLE_ID)) {
		// Its own file, so it can't wipe the log of the Settings app proper.
		char path[64], line[160];
		sceClibSnprintf(path, sizeof(path), DATA_DIR "/log_%s.txt", title_id);
		sceClibSnprintf(line, sizeof(line),
		                "Tweaks/Plugins " TWEAKS_VERSION " was loaded into %s, not the Settings app (" SETTINGS_TITLE_ID
		                "), and did nothing. List tweaks.suprx under *" SETTINGS_TITLE_ID " only.\n",
		                title_id);
		SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd >= 0) {
			sceIoWrite(fd, line, str_len(line));
			sceIoClose(fd);
		}
		return SCE_KERNEL_START_SUCCESS;
	}

	char last_step[100];
	int ended_badly = guard_check(last_step, sizeof(last_step));
	config_load(&cfg);
	log_enable(cfg.log);
	log_rotate();
	SceDateTime now;
	sceClibMemset(&now, 0, sizeof(now));
	sceRtcGetCurrentClockLocalTime(&now);
	log_line("\n== Tweaks/Plugins " TWEAKS_VERSION " in Settings (%s), %04d-%02d-%02d %02d:%02d:%02d\n",
	         ret >= 0 ? title_id : "title id unknown", now.year, now.month, now.day, now.hour, now.minute, now.second);
	log_system(&info);

	if (ended_badly) {
		log_line("Settings closed unexpectedly last time, while: %s. That launch is the one logged above.\n",
		         last_step);
		sceIoRemove(GUARD_PATH);
		int probing = str_starts(last_step, "probing") || str_starts(last_step, "looking for Sony");
		if (probing && !file_exists(NOPROBE_PATH)) {
			// Checking the other pages is the part most likely to differ between
			// firmwares, and Tweaks/Plugins works without it: give that up
			// before giving up altogether.
			switch_set(NOPROBE_PATH);
			sceIoRemove(STRIKE_PATH);
			log_line("that was the up-front check of the other pages, so it is now off\n");
		} else if (file_exists(STRIKE_PATH)) {
			sceIoRemove(STRIKE_PATH);
			switch_set(DISABLED_PATH);
			log_line("that's twice in a row: turning off\n");
		} else {
			write_marker(STRIKE_PATH);
		}
	}
	if (!cfg.enabled) {
		log_line("enabled=0 in " CONFIG_PATH ": staying off\n");
		return SCE_KERNEL_START_SUCCESS;
	}
	if (switch_on(DISABLED_PATH)) {
		log_line("staying off: delete " DISABLED_PATH " to try again\n");
		return SCE_KERNEL_START_SUCCESS;
	}
	if (!cfg.probe)
		no_probe = "probe=0 in " CONFIG_PATH;
	else if (switch_on(NOPROBE_PATH))
		no_probe = "it crashed Settings before; delete " NOPROBE_PATH " to try it again";
	if (no_probe)
		log_line("not checking the other pages up front: %s\n", no_probe);

	list.uid = -1;
	for (int i = 0; i < KEPT_MAX; i++)
		kept[i].uid = -1;

	guard_arm();
	SceUID thid = sceKernelCreateThread("tweaks_guard", guard_thread, 0x10000100, 0x4000, 0, 0, NULL);
	if (thid >= 0)
		sceKernelStartThread(thid, 0, NULL);
	else
		guard_clear(); // nothing would ever clear it

	guard_step("hooking the Settings app");
	load_hook[0] = taiHookFunctionImport(&load_ref[0], "SceSettings", LIB_SCELIBKERNEL, NID_LOAD_START_MODULE,
	                                     load_start_a_patched);
	load_hook[1] = taiHookFunctionImport(&load_ref[1], "SceSettings", LIB_SCELIBKERNEL, NID_LOAD_START_MODULE,
	                                     load_start_b_patched);
	log_line("load hooks 0x%08X,0x%08X\n", load_hook[0], load_hook[1]);
	guard_step("waiting for the settings core");
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
	release(&init_hook, init_ref);
	release(&info_hook, info_ref);
	release(&set_hook, set_ref);
	release(&get_hook, get_ref);
	release(&text_hook, text_ref);
	release(&xml_hook[1], xml_ref[1]);
	release(&xml_hook[0], xml_ref[0]);
	release(&load_hook[1], load_ref[1]);
	release(&load_hook[0], load_ref[0]);
	return SCE_KERNEL_STOP_SUCCESS;
}
