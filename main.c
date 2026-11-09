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
// That one does the work; the other passes straight through. The same trick on
// sceKernelLoadStartModule makes ours the first to see the core load, for the
// case where the XML import can't be hooked until then.
//
// The top menu loads before any other page, so to know what plugins add to the
// other pages we probe them: when the top menu loads, find each page's stock XML
// in the Settings modules' memory, and send it through SceSettings' own import
// stub, so it runs the whole hook chain exactly as if the page were opening.
// Our hook is last, so it sees what every plugin added, takes notes, and returns
// without letting Paf parse it. If no stock XML turns up, the page cache in
// store.c is the fallback.

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
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
		log_line("  %s%s\n", b[i].id, b[i].movable ? "" : " (has buttons, left in place)");
}

static void add_blocks(const char *xml, const tw_block_t *b, int n) {
	for (int i = 0; i < n; i++)
		if (b[i].movable)
			add_entry(b[i].id, xml + b[i].start, b[i].end - b[i].start);
}

// ---- probing the other pages ----

typedef struct {
	const char *xml;
	int size;
	char plugin[48];
} StockPage;

static StockPage stock[STOCK_MAX];
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
	LoadXmlLayout chain = (LoadXmlLayout)import_stub("SceSettings", LIB_SCEPAFMISC, NID_LOAD_XML_LAYOUT);
	int count = find_stock_pages();
	log_line("probe: chain stub %p, %d stock pages\n", chain, count);
	if (!chain || count == 0)
		return 0;

	int probed = 0;
	for (int i = 0; i < count; i++) {
		if (str_eq(stock[i].plugin, ROOT_PLUGIN) || str_eq(stock[i].plugin, HENKAKU_PLUGIN))
			continue;
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

// ---- the pages ----

// The edited XML goes in *done, which the caller frees once Paf has parsed it.
static void edit_root(void *out, void **buf, int *size, void *a4, Mem *done) {
	const char *xml = (const char *)*buf;
	int n = tw_find_blocks(xml, *size, &cfg.rules, tw_match_root, NULL, blocks, TW_MAX_BLOCKS);
	log_line("top menu (%d bytes), %d plugin entries:\n", *size, n);
	log_blocks(blocks, n);

	grouped.count = 0;
	list_len = 0;
	if (mem_alloc(&list, LIST_START) < 0)
		return;
	add_blocks(xml, blocks, n);

	int probed = probe_pages(out, a4);
	if (probed > 0) {
		log_line("probed %d pages\n", probed);
	} else {
		log_line("probing found nothing: using the page cache\n");
		cache_collect(add_entry);
	}

	int len = -1;
	if (mem_alloc(done, *size + list_len + 1024) >= 0)
		len = tw_build_root(xml, *size, blocks, n, list.p, list_len, done->p, done->size);
	mem_free(&list);
	log_line("Tweaks/Plugins holds %d entries; top menu now %d bytes\n", grouped.count, len);
	if (len < 0) {
		mem_free(done);
		grouped.count = 0; // nothing was moved, so other pages must keep theirs
		return;
	}
	*buf = done->p;
	*size = len;
	write_marker(GUARD_PATH); // see top_menu_shown()
}

// ---- crash guard ----
//
// GUARD_PATH exists from building the top menu until it has been on screen for a
// few seconds. If Settings dies in between, the next launch finds it, turns it
// into DISABLED_PATH and leaves Settings alone, so a bad edit can't lock you out.

static int guard_thread(SceSize args, void *argp) {
	sceKernelDelayThread(5 * 1000 * 1000);
	sceIoRemove(GUARD_PATH);
	log_line("top menu stable for 5 s\n");
	return sceKernelExitDeleteThread(0);
}

static void top_menu_shown(int ret) {
	log_line("top menu parsed: 0x%08X\n", ret);
	SceUID thid = sceKernelCreateThread("tweaks_guard", guard_thread, 0x10000100, 0x1000, 0, 0, NULL);
	if (thid >= 0)
		sceKernelStartThread(thid, 0, NULL);
	else
		sceIoRemove(GUARD_PATH);
}

// After Paf has parsed an edited page.
static void parsed(Mem *done, int ret) {
	if (done->uid < 0)
		return;
	if (str_eq(done->page, ROOT_PLUGIN))
		top_menu_shown(ret);
	keep_page(done);
}

static void edit_page(const char *page, void **buf, int *size, Mem *done) {
	const char *xml = (const char *)*buf;

	// The fallback cache only has names to go by.
	int n = tw_find_blocks(xml, *size, &cfg.rules, tw_match_page, NULL, blocks, TW_MAX_BLOCKS);
	int movable = 0;
	for (int i = 0; i < n; i++)
		if (blocks[i].movable)
			blocks[movable++] = blocks[i];
	cache_store(page, xml, blocks, movable);

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
	return NULL;
}

// ---- hooks ----

static SceUID load_hook[2] = {-1, -1}, xml_hook[2] = {-1, -1}, text_hook = -1;
static tai_hook_ref_t load_ref[2], xml_ref[2], text_ref;
static int core_hooked, warned_order;

static int is_last(SceUID uid, tai_hook_ref_t ref) {
	return uid >= 0 && ((struct _tai_hook_user *)ref)->next == 0;
}

// Returns 1 when this call was a probe and must not reach Paf.
static int on_load_xml(int which, void *out, void **buf, int *size, void *a4, Mem *done) {
	done->uid = -1;
	if (is_last(xml_hook[which], xml_ref[which])) {
		if (probe.active) {
			probe_capture((const char *)*buf, *size);
			return 1;
		}
		edit_xml(out, buf, size, a4, done);
	} else if (!is_last(xml_hook[which ^ 1], xml_ref[which ^ 1]) && !warned_order) {
		warned_order = 1;
		log_line("another plugin's XML hook is after ours, so nothing is moved. "
		         "Put tweaks.suprx first under *NPXS10015 in the taiHEN config.\n");
	}
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
	hook_xml();
	text_hook = taiHookFunctionImport(&text_ref, "SceSystemSettingsCore", LIB_SCEPAFTOPLEVEL, NID_GET_TEXT,
	                                  get_text_patched);
	log_line("settings core loaded: xml=0x%08X,0x%08X text=0x%08X\n", xml_hook[0], xml_hook[1], text_hook);
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

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
	tai_module_info_t info;
	info.size = sizeof(info);
	if (taiGetModuleInfo("SceSettings", &info) < 0)
		return SCE_KERNEL_START_SUCCESS; // listed for some other app by mistake: do nothing

	sceIoMkdir(DATA_DIR, 0777);
	sceIoMkdir(CACHE_DIR, 0777);
	log_reset();
	log_line("== Tweaks/Plugins " TWEAKS_VERSION " in Settings\n");
	config_load(&cfg);
	if (!cfg.enabled) {
		log_line("enabled=0 in " CONFIG_PATH ": staying off\n");
		return SCE_KERNEL_START_SUCCESS;
	}
	if (file_exists(GUARD_PATH)) {
		sceIoRename(GUARD_PATH, DISABLED_PATH);
		log_line("Settings closed unexpectedly last time, just after the top menu was built\n");
	}
	if (file_exists(DISABLED_PATH)) {
		log_line("staying off: delete " DISABLED_PATH " to try again\n");
		return SCE_KERNEL_START_SUCCESS;
	}

	list.uid = -1;
	for (int i = 0; i < KEPT_MAX; i++)
		kept[i].uid = -1;

	load_hook[0] = taiHookFunctionImport(&load_ref[0], "SceSettings", LIB_SCELIBKERNEL, NID_LOAD_START_MODULE,
	                                     load_start_a_patched);
	load_hook[1] = taiHookFunctionImport(&load_ref[1], "SceSettings", LIB_SCELIBKERNEL, NID_LOAD_START_MODULE,
	                                     load_start_b_patched);
	// Hooking the XML import now, before HENkaku and Engram do theirs at core
	// load, keeps ours at the end of the chain whatever order the config lists
	// them in. If it isn't hookable yet, core_loaded() tries again.
	hook_xml();
	log_line("load hooks 0x%08X,0x%08X; early xml hooks 0x%08X,0x%08X\n", load_hook[0], load_hook[1], xml_hook[0],
	         xml_hook[1]);
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
	release(&text_hook, text_ref);
	release(&xml_hook[1], xml_ref[1]);
	release(&xml_hook[0], xml_ref[0]);
	release(&load_hook[1], load_ref[1]);
	release(&load_hook[0], load_ref[0]);
	return SCE_KERNEL_STOP_SUCCESS;
}
