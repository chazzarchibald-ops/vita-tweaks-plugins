#include "regroup.h"

#ifdef TW_HOST
#include <string.h>
#define tw_memcpy memcpy
#else
#include <psp2/kernel/clib.h>
#define tw_memcpy sceClibMemcpy
#endif

// Sony's top-menu rows (3.60 list HENkaku ships as its system_settings.xml).
static const char *const g_stock_root_ids[] = {
	"system_settings", "sandbox_settings", "framework_test", "spacer0", "flight_mode",
	"henkaku_settings", "network_settings", "psn_settings", "peripherals_settings",
	"notification_settings", "sound_settings", "theme_settings", "location_settings",
	"security_settings", "datetime_settings", "language_settings", "accessibility",
	"console_info", "reset", "powersave_settings", "idu_settings", "debug_settings",
};

static const char g_list_open[] =
	"\n<setting_list id=\"tweaks_plugins\" title=\"tw_title\" icon=\"tex_spanner\">\n";
static const char g_list_empty[] =
	"<label id=\"tw_empty\" title=\"tw_empty_title\" description=\"tw_empty_text\"/>\n";
static const char g_list_close[] = "</setting_list>\n";

static int str_len(const char *s) {
	int n = 0;
	while (s[n]) n++;
	return n;
}

static int str_eq(const char *a, const char *b) {
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static int str_prefix(const char *s, const char *prefix) {
	while (*prefix) if (*s++ != *prefix++) return 0;
	return 1;
}

static int str_contains(const char *s, const char *needle) {
	for (; *s; s++) if (str_prefix(s, needle)) return 1;
	return 0;
}

// True when [p, end) starts with s.
static int at(const char *p, const char *end, const char *s) {
	for (; *s; p++, s++) if (p >= end || *p != *s) return 0;
	return 1;
}

static const char *find(const char *p, const char *end, const char *s) {
	for (; p < end; p++) if (at(p, end, s)) return p;
	return 0;
}

static int is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// True when p is "<name" followed by whitespace, '/' or '>'.
static int at_tag(const char *p, const char *end, const char *name) {
	if (p >= end || *p != '<' || !at(p + 1, end, name)) return 0;
	p += 1 + str_len(name);
	return p < end && (is_space(*p) || *p == '/' || *p == '>');
}

// Returns the '>' closing the tag that starts at p, skipping quoted values.
static const char *tag_close(const char *p, const char *end) {
	char quote = 0;
	for (; p < end; p++) {
		if (quote) { if (*p == quote) quote = 0; }
		else if (*p == '"' || *p == '\'') quote = *p;
		else if (*p == '>') return p;
	}
	return 0;
}

static void get_attr(const char *tag, const char *tag_end, const char *name, char *out, int out_size) {
	int n = str_len(name);
	out[0] = '\0';
	for (const char *p = tag + 1; p < tag_end; p++) {
		if (!is_space(p[-1]) || !at(p, tag_end, name)) continue;
		const char *q = p + n;
		while (q < tag_end && is_space(*q)) q++;
		if (q >= tag_end || *q++ != '=') continue;
		while (q < tag_end && is_space(*q)) q++;
		if (q >= tag_end || (*q != '"' && *q != '\'')) continue;
		char quote = *q++;
		int i = 0;
		while (q < tag_end && *q != quote && i < out_size - 1) out[i++] = *q++;
		out[i] = '\0';
		return;
	}
}

// p is at "<name ...". Returns the end of the whole element (past its closing '>'), or 0.
static const char *element_end(const char *p, const char *end, const char *name) {
	const char *gt = tag_close(p, end);
	if (!gt) return 0;
	if (gt[-1] == '/') return gt + 1;
	int depth = 1;
	int n = str_len(name);
	for (p = gt + 1; p < end; p++) {
		if (*p != '<') continue;
		if (at(p, end, "<!--")) {
			const char *c = find(p, end, "-->");
			if (!c) return 0;
			p = c + 2;
		} else if (at(p + 1, end, "/") && at(p + 2, end, name) && p + 2 + n < end &&
			           (p[2 + n] == '>' || is_space(p[2 + n]))) {
			const char *c = tag_close(p, end);
			if (!c) return 0;
			if (--depth == 0) return c + 1;
			p = c;
		} else if (at_tag(p, end, name)) {
			const char *c = tag_close(p, end);
			if (!c) return 0;
			if (c[-1] != '/') depth++;
			p = c;
		}
	}
	return 0;
}

int tw_idset_has(const tw_idset_t *set, const char *id) {
	for (int i = 0; i < set->count; i++) if (str_eq(set->id[i], id)) return 1;
	return 0;
}

void tw_idset_add(tw_idset_t *set, const char *id) {
	if (set->count >= TW_MAX_IDS || tw_idset_has(set, id)) return;
	int i = 0;
	for (; id[i] && i < TW_ID_LEN - 1; i++) set->id[set->count][i] = id[i];
	set->id[set->count][i] = '\0';
	set->count++;
}

static int is_stock_root(const char *id) {
	for (unsigned i = 0; i < sizeof(g_stock_root_ids) / sizeof(*g_stock_root_ids); i++)
		if (str_eq(g_stock_root_ids[i], id)) return 1;
	return 0;
}

int tw_match_root(const char *id, const char *title, void *ctx) {
	return !str_prefix(id, "id_") && !is_stock_root(id);
}

int tw_match_page(const char *id, const char *title, void *ctx) {
	return !str_prefix(id, "id_") && !str_prefix(title, "msg_");
}

int tw_match_ids(const char *id, const char *title, void *ctx) {
	return tw_idset_has((const tw_idset_t *)ctx, id);
}

// The rules every match goes through first: HENkaku's and our own entries never
// move, keep= never moves, move= always does.
static int is_foreign(const char *id, const char *title, const tw_rules_t *rules, tw_match_t match, void *ctx) {
	if (!id[0]) return 0;
	if (str_contains(id, "henkaku") || str_prefix(id, "tweaks_") || str_prefix(id, "tw_")) return 0;
	if (tw_idset_has(&rules->keep, id)) return 0;
	if (tw_idset_has(&rules->move, id)) return 1;
	return match(id, title, ctx);
}

int tw_page_plugin(const char *xml, int size, char *out, int out_size) {
	const char *end = xml + (size < 512 ? size : 512);
	const char *p = find(xml, end, "plugin=\"");
	out[0] = '\0';
	if (!p) return 0;
	p += 8;
	int i = 0;
	while (p < end && *p != '"' && i < out_size - 1) out[i++] = *p++;
	out[i] = '\0';
	return p < end;
}

int tw_find_blocks(const char *xml, int size, const tw_rules_t *rules, tw_match_t match, void *ctx,
                   tw_block_t *blocks, int max_blocks) {
	const char *end = xml + size;
	int depth = 0;   // how many <setting_list> we're inside; the page's own list is depth 0
	int count = 0;
	char id[TW_ID_LEN], title[TW_ID_LEN];

	for (const char *p = xml; p < end; p++) {
		if (*p != '<') continue;
		if (at(p, end, "<!--")) {
			const char *c = find(p, end, "-->");
			if (!c) break;
			p = c + 2;
			continue;
		}
		if (at(p, end, "</setting_list")) {
			depth--;
			continue;
		}
		int is_list = at_tag(p, end, "setting_list");
		if (!is_list && !at_tag(p, end, "link")) continue;

		const char *gt = tag_close(p, end);
		if (!gt) break;
		get_attr(p, gt, "id", id, sizeof(id));
		get_attr(p, gt, "title", title, sizeof(title));

		if (depth >= 1 && count < max_blocks && is_foreign(id, title, rules, match, ctx)) {
			const char *e = element_end(p, end, is_list ? "setting_list" : "link");
			if (!e) break;
			tw_block_t *b = &blocks[count++];
			b->start = (int)(p - xml);
			b->end = (int)(e - xml);
			b->movable = tw_idset_has(&rules->move, id) ||
		             (!find(p, e, "<button") && !find(p, e, "<user_custom"));
			for (int i = 0; i < TW_ID_LEN; i++) b->id[i] = id[i];
			p = e - 1;
			continue;
		}
		if (is_list && gt[-1] != '/') depth++;
		p = gt;
	}
	return count;
}

static int put(char *out, int out_size, int *pos, const char *s, int n) {
	if (n <= 0) return 0;
	if (*pos + n > out_size) return -1;
	tw_memcpy(out + *pos, s, n);
	*pos += n;
	return 0;
}

// Copies xml[from, to) minus every movable block inside that range.
static int put_without(const char *xml, int from, int to, const tw_block_t *blocks, int nblocks,
                       char *out, int out_size, int *pos) {
	int cur = from;
	for (int i = 0; i < nblocks; i++) {
		const tw_block_t *b = &blocks[i];
		if (!b->movable || b->end <= cur || b->start >= to) continue;
		if (put(out, out_size, pos, xml + cur, b->start - cur) < 0) return -1;
		cur = b->end;
	}
	return put(out, out_size, pos, xml + cur, to - cur);
}

// Where the Tweaks/Plugins row goes: after HENkaku's row, else after airplane mode,
// else first in the top menu.
static int root_insert_point(const char *xml, int size) {
	static const char *const anchors[][2] = {
		{ "henkaku_settings", "link" }, { "flight_mode", "toggle_switch" },
	};
	const char *end = xml + size;
	char id[TW_ID_LEN];
	for (unsigned a = 0; a < sizeof(anchors) / sizeof(*anchors); a++) {
		for (const char *p = xml; (p = find(p, end, "<")); p++) {
			if (!at_tag(p, end, anchors[a][1])) continue;
			const char *gt = tag_close(p, end);
			if (!gt) break;
			get_attr(p, gt, "id", id, sizeof(id));
			if (str_eq(id, anchors[a][0])) {
				const char *e = element_end(p, end, anchors[a][1]);
				if (e) return (int)(e - xml);
			}
		}
	}
	for (const char *p = xml; (p = find(p, end, "<")); p++) {
		if (at_tag(p, end, "setting_list")) {
			const char *gt = tag_close(p, end);
			return gt ? (int)(gt + 1 - xml) : -1;
		}
	}
	return -1;
}

int tw_build_root(const char *xml, int size, const tw_block_t *blocks, int nblocks,
                  const char *extra, int extra_len, char *out, int out_size) {
	int ins = root_insert_point(xml, size);
	if (ins < 0) return -1;
	int pos = 0;

	if (put_without(xml, 0, ins, blocks, nblocks, out, out_size, &pos) < 0) return -1;
	if (put(out, out_size, &pos, g_list_open, sizeof(g_list_open) - 1) < 0) return -1;
	if (put(out, out_size, &pos, extra, extra_len) < 0) return -1;
	if (extra_len == 0 &&
			put(out, out_size, &pos, g_list_empty, sizeof(g_list_empty) - 1) < 0)
		return -1;
	if (put(out, out_size, &pos, g_list_close, sizeof(g_list_close) - 1) < 0) return -1;
	if (put_without(xml, ins, size, blocks, nblocks, out, out_size, &pos) < 0) return -1;
	return pos;
}

int tw_retitle(const char *block, int len, const char *title, char *out, int out_size) {
	const char *end = block + len;
	const char *gt = tag_close(block, end);
	int pos = 0;
	if (!gt) return -1;

	const char *value = 0, *value_end = 0;
	for (const char *p = block + 1; p < gt; p++) {
		if (!is_space(p[-1]) || !at(p, gt, "title=\"")) continue;
		value = p + 7;
		value_end = find(value, gt, "\"");
		break;
	}
	if (value && value_end) {
		if (put(out, out_size, &pos, block, (int)(value - block)) < 0 ||
		    put(out, out_size, &pos, title, str_len(title)) < 0 ||
		    put(out, out_size, &pos, value_end, (int)(end - value_end)) < 0)
			return -1;
		return pos;
	}
	// No title yet: add one straight after the tag name.
	const char *name_end = block + 1;
	while (name_end < gt && !is_space(*name_end) && *name_end != '/' && *name_end != '>') name_end++;
	if (put(out, out_size, &pos, block, (int)(name_end - block)) < 0 ||
	    put(out, out_size, &pos, " title=\"", 8) < 0 ||
	    put(out, out_size, &pos, title, str_len(title)) < 0 ||
	    put(out, out_size, &pos, "\"", 1) < 0 ||
	    put(out, out_size, &pos, name_end, (int)(end - name_end)) < 0)
		return -1;
	return pos;
}

int tw_strip_blocks(const char *xml, int size, const tw_block_t *blocks, int nblocks,
                    char *out, int out_size) {
	int pos = 0;
	if (put_without(xml, 0, size, blocks, nblocks, out, out_size, &pos) < 0) return -1;
	return pos;
}
