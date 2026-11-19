#ifndef TWEAKS_REGROUP_H
#define TWEAKS_REGROUP_H

// Pure XML text work for Tweaks/Plugins: no hooks, no file IO. Everything works on
// byte ranges of the buffer Settings hands to scePafMiscLoadXmlLayout, so it can be
// unit tested on a PC by defining TW_HOST.

#define TW_MAX_IDS    64
#define TW_ID_LEN     64
#define TW_MAX_BLOCKS 32

typedef struct {
	int start, end;          // [start, end) of the whole element in the page buffer
	int kept;                // config says keep= it where its plugin put it
	int custom;              // holds a <user_custom>, a widget drawn by the page it's on
	int movable;             // goes to Tweaks/Plugins: not kept, and not custom unless move= says so
	int buttons;             // holds a <button>: these have worked after a move (Wi-Fi Turbo's), but may not
	char id[TW_ID_LEN];
	char title[TW_ID_LEN];   // its title attribute: a text id, or the text itself
} tw_block_t;

typedef struct {
	int count;
	char id[TW_MAX_IDS][TW_ID_LEN];
} tw_idset_t;

typedef struct {
	tw_idset_t move;         // config "move=<id>": always group this entry
	tw_idset_t keep;         // config "keep=<id>": never group this entry
} tw_rules_t;

// Copies the page's plugin="..." name into out. Returns 1 if found.
int tw_page_plugin(const char *xml, int size, char *out, int out_size);

// Decides whether a <setting_list>/<link> with this id and title is a plugin's.
typedef int (*tw_match_t)(const char *id, const char *title, void *ctx);

// Guesses from names, for when there is no stock page to compare with: top menu,
// an id that is not one of Sony's rows; other pages, a title that is not one of
// Sony's "msg_" text ids (plugins bring their own text, or write it out as YAMT does).
int tw_match_root(const char *id, const char *title, void *ctx);
int tw_match_page(const char *id, const char *title, void *ctx);
// ctx is a tw_idset_t: exactly those ids.
int tw_match_ids(const char *id, const char *title, void *ctx);

// Whether a config line's value names this entry. People write what they see, so
// case and punctuation are ignored, and it may be the id ("id_yamt"), the id
// without "id_" or "_settings" ("yamt", "Wi-Fi Turbo" for wifiturbo_settings), or
// the entry's own title ("Storage Devices").
int tw_name_matches(const char *want, const char *id, const char *title);
int tw_rule_has(const tw_idset_t *set, const char *id, const char *title);

// Finds the entries below the page's own list that `match` picks, after the rules
// every page shares: HENkaku's and our own never, move= always. keep= entries are
// found too, marked kept and not movable. A match is taken whole, nested lists and
// all. Returns the block count.
int tw_find_blocks(const char *xml, int size, const tw_rules_t *rules, tw_match_t match, void *ctx,
                   tw_block_t *blocks, int max_blocks);

// Copies one block with its own title="..." set to `title`. Returns the size, or -1.
int tw_retitle(const char *block, int len, const char *title, char *out, int out_size);

// The attribute `name` of the tag that starts at `tag`, or "" if it has none.
void tw_tag_attr(const char *tag, int len, const char *name, char *out, int out_size);
int tw_has(const char *text, int len, const char *needle);

// Writes the root menu with the movable `blocks` removed and a "Tweaks/Plugins"
// submenu inserted right after HENkaku's row. The submenu holds `head` (our own
// rows), then `extra` (every plugin entry for the list, the removed blocks among
// them, as the caller wants them shown), or a note that there are none. Returns
// the output size, or -1 if out is too small.
int tw_build_root(const char *xml, int size, const tw_block_t *blocks, int nblocks,
                  const char *head, int head_len, const char *extra, int extra_len,
                  char *out, int out_size);

// Writes the page with `blocks` removed. Returns the output size, or -1.
int tw_strip_blocks(const char *xml, int size, const tw_block_t *blocks, int nblocks,
                    char *out, int out_size);

int tw_idset_has(const tw_idset_t *set, const char *id);
void tw_idset_add(tw_idset_t *set, const char *id);

#endif
