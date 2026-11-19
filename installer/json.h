// Just enough JSON to read what GitHub says of a release: the value of a key,
// found by searching the text. That is safe here because the answers come
// from GitHub over a checked connection and are only ever compared or checked
// (a version, a file name, a size, a digest, a link that must start with
// GitHub's own address).
#ifndef JSON_H
#define JSON_H

#include <stdbool.h>

// The first "key" at or after from, and where its value starts. NULL if there is none.
const char *json_value(const char *from, const char *key);
// The string that "key" has, into out. False if it has none, or it doesn't fit.
bool json_string(const char *from, const char *key, char *out, int size);
// The whole number that "key" has, or -1.
int json_number(const char *from, const char *key);

// For longer text (a release's notes): the string that "key" has, with what
// JSON writes as \n, \t, \" and so on turned back into the characters, and
// \uXXXX into UTF-8. What doesn't fit is left off. False if it has no string.
bool json_text(const char *from, const char *key, char *out, int size);

// The end of the value that starts at p (a string, a number, a list, an
// object, true, false or null): where what follows it begins. NULL if it
// doesn't end. For stepping through a list an item at a time.
const char *json_skip(const char *p);

#endif
