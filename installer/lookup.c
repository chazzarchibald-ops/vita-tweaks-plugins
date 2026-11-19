#include "lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

bool lookup_asset(const char *answer, const char *repo, const char *name, ReleaseFile *file) {
	char downloads[200], found[80], digest[80] = "";
	memset(file, 0, sizeof(*file));
	snprintf(downloads, sizeof(downloads), "https://github.com/%s/releases/download/", repo);
	bool ends = name[0] == '*';
	const char *want = ends ? name + 1 : name, *asset = NULL;
	for (const char *at = answer; (at = strstr(at, "\"name\"")) != NULL; at++) {
		if (!json_string(at, "name", found, sizeof(found)))
			continue;
		size_t a = strlen(found), b = strlen(want);
		if (ends ? a > b && strcmp(found + a - b, want) == 0 : strcmp(found, want) == 0) {
			asset = at;
			break;
		}
	}
	if (!asset)
		return false;
	json_string(answer, "tag_name", file->tag, sizeof(file->tag));
	snprintf(file->name, sizeof(file->name), "%s", found);
	file->size = json_number(asset, "size");
	json_string(asset, "digest", digest, sizeof(digest));
	json_string(asset, "browser_download_url", file->url, sizeof(file->url));
	if (strncmp(file->url, downloads, strlen(downloads)) != 0 || file->size <= 0 || strncmp(digest, "sha256:", 7) != 0 ||
	    strlen(digest + 7) != 64)
		return false;
	memcpy(file->digest, digest + 7, 65);
	return true;
}

bool lookup_release(const char *answer, char *name, int name_size, char date[16]) {
	static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	char tag[64], raw[160] = "", when[40] = "";
	date[0] = '\0';
	if (name_size < 2 || !json_string(answer, "tag_name", tag, sizeof(tag)))
		return false;
	// the release's own name comes after its tag (an asset's name comes later still)
	const char *after = json_value(answer, "tag_name");
	const char *assets = strstr(after, "\"assets\"");
	const char *named = json_value(after, "name");
	if (named && (!assets || named < assets))
		json_string(after, "name", raw, sizeof(raw));
	if (!raw[0])
		snprintf(raw, sizeof(raw), "%s", tag);
	// in plain letters: anything else (a long dash, say) becomes a hyphen
	int n = 0;
	for (const unsigned char *c = (const unsigned char *)raw; *c && n < name_size - 1; c++) {
		if (*c >= 0x80) {
			if (*c >= 0xC0) // the first byte of a letter outside ASCII; the rest of it is skipped
				name[n++] = '-';
		} else if (*c >= 0x20) {
			name[n++] = (char)*c;
		}
	}
	name[n] = '\0';
	// "2026-11-06T..." as "6 Nov 2026"
	if (json_string(answer, "published_at", when, sizeof(when)) && strlen(when) >= 10 && when[4] == '-' && when[7] == '-') {
		int month = (when[5] - '0') * 10 + (when[6] - '0'), day = (when[8] - '0') * 10 + (when[9] - '0');
		if (month >= 1 && month <= 12 && day >= 1 && day <= 31)
			snprintf(date, 16, "%d %s %.4s", day, months[month - 1], when);
	}
	return true;
}

void lookup_size_text(int bytes, char out[12]) {
	if (bytes < 1024 * 1024) {
		snprintf(out, 12, "%d KB", (bytes + 1023) / 1024);
	} else if (bytes < 100 * 1024 * 1024) {
		int tenths = (bytes + 52429) / 104858; // tenths of a megabyte, to the nearest
		snprintf(out, 12, "%d.%d MB", tenths / 10, tenths % 10);
	} else {
		snprintf(out, 12, "%u MB", ((unsigned)bytes + 524288u) / 1048576u);
	}
}

static bool ends_png(const char *name) {
	size_t n = strlen(name);
	if (n < 5)
		return false;
	const char *e = name + n - 4;
	return e[0] == '.' && (e[1] == 'p' || e[1] == 'P') && (e[2] == 'n' || e[2] == 'N') && (e[3] == 'g' || e[3] == 'G');
}

int lookup_shots(const char *answer, const char *repo, Shot *out, int max) {
	char raw[200], name[128], type[16];
	snprintf(raw, sizeof(raw), "https://raw.githubusercontent.com/%s/", repo);
	const char *p = answer;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (*p != '[')
		return -1; // a folder's answer is a list; anything else is an error, or a single file
	int n = 0;
	// each file's details follow its name: path, sha, size, links, type
	for (const char *at = answer; n < max && (at = strstr(at, "\"name\"")) != NULL; at++) {
		if (!json_string(at, "name", name, sizeof(name)) || !ends_png(name))
			continue;
		Shot *s = &out[n];
		memset(s, 0, sizeof(*s));
		const char *next = strstr(at + 1, "\"name\"");
		const char *sha = json_value(at, "sha"), *url = json_value(at, "download_url"), *kind = json_value(at, "type");
		// (each must be this file's own, not a later one's)
		if (!sha || !url || !kind || (next && (sha > next || url > next || kind > next)))
			continue;
		if (!json_string(at, "sha", s->sha, sizeof(s->sha)) || !json_string(at, "download_url", s->url, sizeof(s->url)) ||
		    !json_string(at, "type", type, sizeof(type)) || strcmp(type, "file") != 0)
			continue;
		s->size = json_number(at, "size");
		bool hex = strlen(s->sha) == 40;
		for (int i = 0; hex && i < 40; i++)
			hex = (s->sha[i] >= '0' && s->sha[i] <= '9') || (s->sha[i] >= 'a' && s->sha[i] <= 'f');
		if (!hex || s->size <= 0 || strncmp(s->url, raw, strlen(raw)) != 0)
			continue;
		n++;
	}
	return n;
}

// ---- a release's notes ----

#define LINE_MAX 1600 // of a line of notes: a long item of a list is one line

// A character outside ASCII as plain letters, appended to out (which has room for 3 more).
static int plain_char(unsigned code, char *out) {
	// U+00C0 to U+017F: the Latin letters with marks on them, as the letters without
	static const char latin[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
	                            "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTt"
	                            "UuUuUuUuUuUuWwYyYZzZzZzs";
	const char *as = "";
	char one[2] = {0, 0};
	if (code >= 0xC0 && code < 0x180) {
		one[0] = latin[code - 0xC0];
		as = one;
	} else if (code == 0xA0) {
		as = " ";
	} else if (code == 0x2018 || code == 0x2019) {
		as = "'";
	} else if (code == 0x201C || code == 0x201D) {
		as = "\"";
	} else if (code == 0x2013 || code == 0x2014 || code == 0x2022) {
		as = "-";
	} else if (code == 0x2026) {
		as = "...";
	} else if (code == 0x2192) {
		as = "->";
	} else if (code == 0x2190) {
		as = "<-";
	}
	int n = (int)strlen(as);
	memcpy(out, as, (size_t)n);
	return n;
}

// One line of notes with its markdown taken off, into out (of size bytes): no
// backticks or ** marks, a link as its words, no pictures or HTML tags, and
// plain letters only. Returns its length.
static int plain_line(const char *s, int len, char *out, int size) {
	int n = 0;
	for (int i = 0; i < len && n < size - 4;) {
		unsigned char c = (unsigned char)s[i];
		if (c == '`') {
			i++;
		} else if (c == '*' && i + 1 < len && s[i + 1] == '*') {
			i += 2;
		} else if (c == '!' && i + 1 < len && s[i + 1] == '[') { // a picture: nothing of it is kept
			int close = i + 2;
			while (close < len && s[close] != ']')
				close++;
			int end = close + 1;
			if (close < len && end < len && s[end] == '(') {
				while (end < len && s[end] != ')')
					end++;
				i = end < len ? end + 1 : len;
			} else {
				out[n++] = '!';
				i++;
			}
		} else if (c == '[') { // a link: its words are kept, its address isn't
			int close = i + 1;
			while (close < len && s[close] != ']')
				close++;
			int end = close + 1;
			if (close < len && end < len && s[end] == '(') {
				while (end < len && s[end] != ')')
					end++;
			} else {
				end = len;
			}
			if (end < len) {
				for (int k = i + 1; k < close && n < size - 4; k++)
					if (s[k] != '`' && s[k] != '*' && (unsigned char)s[k] < 0x80)
						out[n++] = s[k];
				i = end + 1;
			} else {
				out[n++] = '[';
				i++;
			}
		} else if (c == '<' && i + 1 < len &&
		           (s[i + 1] == '/' || (s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z'))) {
			int end = i + 1; // an HTML tag, if it closes
			while (end < len && s[end] != '>')
				end++;
			if (end < len) {
				i = end + 1;
			} else {
				out[n++] = '<';
				i++;
			}
		} else if (c >= 0x80) { // UTF-8: two, three or four bytes of one character
			int count = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
			unsigned code = count == 4 ? c & 0x07u : count == 3 ? c & 0x0Fu : count == 2 ? c & 0x1Fu : 0;
			for (int k = 1; k < count && i + k < len; k++)
				code = (code << 6) | ((unsigned char)s[i + k] & 0x3Fu);
			if (count > 1)
				n += plain_char(code, out + n);
			i += count;
		} else if (c < 0x20) {
			out[n++] = ' '; // a tab, say
			i++;
		} else {
			out[n++] = (char)c;
			i++;
		}
	}
	while (n && out[n - 1] == ' ')
		n--;
	out[n] = '\0';
	return n;
}

// A release's notes as plain lines in out (of size bytes), a newline between
// them. A line that starts with \1 is a heading and one that starts with \2
// an item of a list; an empty line is a gap. GitHub shows each line of a
// release's notes as a line, so they are kept as they are.
static void plain_notes(const char *notes, char *out, int size) {
	static char line[LINE_MAX + 8]; // (big, and only the lookups' thread reads notes)
	int n = 0;
	bool fenced = false, gap = false;
	out[0] = '\0';
	for (const char *p = notes; *p;) {
		const char *end = p;
		while (*end && *end != '\n' && *end != '\r')
			end++;
		const char *s = p;
		int len = (int)(end - p);
		p = *end == '\r' && end[1] == '\n' ? end + 2 : *end ? end + 1 : end;
		while (len && (*s == ' ' || *s == '\t')) {
			s++;
			len--;
		}
		while (len && (s[len - 1] == ' ' || s[len - 1] == '\t'))
			len--;
		if (len >= 3 && memcmp(s, "```", 3) == 0) { // the marks around a block of code
			fenced = !fenced;
			continue;
		}
		char mark = 0;
		int k = 0;
		if (!fenced && len && s[0] == '#') {
			while (len && *s == '#') {
				s++;
				len--;
			}
			while (len && *s == ' ') {
				s++;
				len--;
			}
			mark = 1;
		} else if (!fenced && len >= 2 && (s[0] == '*' || s[0] == '-' || s[0] == '+') && s[1] == ' ') {
			s += 2;
			len -= 2;
			while (len && *s == ' ') {
				s++;
				len--;
			}
			mark = 2;
		}
		bool had = len > 0;
		if (len > LINE_MAX - 8)
			len = LINE_MAX - 8;
		k = fenced ? 0 : plain_line(s, len, line, sizeof(line));
		if (fenced) { // code is shown as it is, less anything outside ASCII
			for (int i = 0; i < len; i++)
				if ((unsigned char)s[i] >= 0x20 && (unsigned char)s[i] < 0x80)
					line[k++] = s[i];
			line[k] = '\0';
		}
		if (!k) {
			if (!had && n)
				gap = true; // an empty line; a line that was only a picture or a tag leaves nothing
			continue;
		}
		int need = k + (mark ? 1 : 0) + (gap ? 1 : 0) + (n ? 1 : 0);
		if (n + need >= size - 5) { // no room: it ends here, and says so
			memcpy(out + n, "\n...", 5);
			return;
		}
		if (n)
			out[n++] = '\n';
		if (gap)
			out[n++] = '\n';
		gap = false;
		if (mark)
			out[n++] = mark;
		memcpy(out + n, line, (size_t)k);
		n += k;
		out[n] = '\0';
	}
}

int lookup_releases(const char *answer, const char *repo, const char *const *assets, int asset_count, Release *out, int max) {
	const char *p = answer;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (*p != '[')
		return -1;
	p++;
	int n = 0;
	char *notes = malloc(NOTES_RAW);
	if (!notes)
		return -1;
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')
			p++;
		if (*p != '{')
			break; // the end of the list
		const char *end = json_skip(p);
		if (!end)
			break;
		// this release on its own, so that nothing is taken from the next one
		size_t len = (size_t)(end - p);
		char *one = malloc(len + 1);
		if (!one)
			break;
		memcpy(one, p, len);
		one[len] = '\0';
		p = end;
		const char *draft = json_value(one, "draft"), *pre = json_value(one, "prerelease");
		Release *r = &out[n];
		memset(r, 0, sizeof(*r));
		if (n < max && draft && *draft == 'f' && json_string(one, "tag_name", r->tag, sizeof(r->tag)) &&
		    lookup_release(one, r->name, sizeof(r->name), r->date)) {
			r->prerelease = pre && *pre == 't';
			for (int i = 0; i < asset_count && i < RELEASE_FILES; i++) {
				ReleaseFile file;
				if (assets[i] && lookup_asset(one, repo, assets[i], &file))
					lookup_size_text(file.size, r->size[i]);
			}
			if (json_text(one, "body", notes, NOTES_RAW))
				plain_notes(notes, r->notes, sizeof(r->notes));
			n++;
		}
		free(one);
	}
	free(notes);
	return n;
}
