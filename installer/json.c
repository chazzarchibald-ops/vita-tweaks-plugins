#include "json.h"

#include <stdio.h>
#include <string.h>

const char *json_value(const char *from, const char *key) {
	char quoted[40];
	snprintf(quoted, sizeof(quoted), "\"%s\"", key);
	for (const char *at = strstr(from, quoted); at; at = strstr(at + 1, quoted)) {
		const char *p = at + strlen(quoted);
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p != ':')
			continue; // the word as a value, not a key
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		return p;
	}
	return NULL;
}

bool json_string(const char *from, const char *key, char *out, int size) {
	const char *p = json_value(from, key);
	if (!p || *p != '"')
		return false;
	int n = 0;
	for (p++; *p && *p != '"'; p++) {
		if (*p == '\\' && p[1])
			p++; // \" and \/ and \\ are themselves; nothing else is expected here
		if (n >= size - 1)
			return false;
		out[n++] = *p;
	}
	out[n] = '\0';
	return *p == '"';
}

int json_number(const char *from, const char *key) {
	const char *p = json_value(from, key);
	if (!p || *p < '0' || *p > '9')
		return -1;
	int value = 0;
	for (; *p >= '0' && *p <= '9' && value < 100000000; p++)
		value = value * 10 + (*p - '0');
	return value;
}

static int hex_digit(char c) {
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

bool json_text(const char *from, const char *key, char *out, int size) {
	const char *p = json_value(from, key);
	if (!p || *p != '"' || size < 1)
		return false;
	int n = 0;
	for (p++; *p && *p != '"'; p++) {
		unsigned code = (unsigned char)*p;
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': code = '\n'; break;
			case 'r': code = '\r'; break;
			case 't': code = '\t'; break;
			case 'b': code = ' '; break;
			case 'f': code = ' '; break;
			case 'u': {
				code = 0;
				for (int i = 1; i <= 4; i++) {
					int d = hex_digit(p[i]);
					if (d < 0)
						return false;
					code = code * 16 + (unsigned)d;
				}
				p += 4;
				break;
			}
			default: code = (unsigned char)*p; // \" \\ \/ are themselves
			}
		}
		// as UTF-8 (a byte that came as it was is already part of some)
		char bytes[3];
		int count = 1;
		if (code < 0x80 || (unsigned char)*p >= 0x80) {
			bytes[0] = (char)code;
		} else if (code < 0x800) {
			bytes[0] = (char)(0xC0 | (code >> 6));
			bytes[1] = (char)(0x80 | (code & 0x3F));
			count = 2;
		} else {
			bytes[0] = (char)(0xE0 | (code >> 12));
			bytes[1] = (char)(0x80 | ((code >> 6) & 0x3F));
			bytes[2] = (char)(0x80 | (code & 0x3F));
			count = 3;
		}
		if (n + count < size) {
			memcpy(out + n, bytes, (size_t)count);
			n += count;
		}
	}
	out[n] = '\0';
	return *p == '"';
}

const char *json_skip(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (*p == '"') {
		for (p++; *p && *p != '"'; p++)
			if (*p == '\\' && p[1])
				p++;
		return *p ? p + 1 : NULL;
	}
	if (*p == '{' || *p == '[') {
		int depth = 0;
		for (; *p; p++) {
			if (*p == '"') { // a string may have brackets in it
				p = json_skip(p);
				if (!p)
					return NULL;
				p--;
			} else if (*p == '{' || *p == '[') {
				depth++;
			} else if (*p == '}' || *p == ']') {
				if (--depth == 0)
					return p + 1;
			}
		}
		return NULL;
	}
	const char *start = p;
	while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
		p++;
	return p > start ? p : NULL;
}
