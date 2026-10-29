/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "glob.h"
#include <stddef.h>

static int protected_at(const unsigned char *quoted, size_t index)
{
    return quoted != NULL && quoted[index] != 0;
}

/* Return the index after a valid bracket expression, or zero if malformed. */
static size_t bracket_end(const char *pattern, const unsigned char *quoted, size_t start)
{
    size_t i = start + 1;
    if (pattern[i] == '!' && !protected_at(quoted, i)) i++;
    if (pattern[i] == ']') i++;
    for (; pattern[i] != '\0'; i++) {
        if (pattern[i] == ']' && !protected_at(quoted, i)) return i + 1;
    }
    return 0;
}

static int bracket_match(const char *pattern, const unsigned char *quoted,
                         size_t start, size_t end, unsigned char c)
{
    size_t i = start + 1;
    int negate = 0, matched = 0;
    if (pattern[i] == '!' && !protected_at(quoted, i)) {
        negate = 1;
        i++;
    }
    while (i < end - 1) {
        unsigned char first = (unsigned char)pattern[i++];
        if (i + 1 < end - 1 && pattern[i] == '-' && !protected_at(quoted, i)) {
            unsigned char last = (unsigned char)pattern[i + 1];
            if (first <= c && c <= last) matched = 1;
            i += 2;
        } else if (first == c) {
            matched = 1;
        }
    }
    return negate ? !matched : matched;
}

int shell_glob_match(const char *pattern, const unsigned char *quoted, const char *name)
{
    size_t p = 0, n = 0, star = (size_t)-1, retry = 0;
    if (pattern == NULL || name == NULL) return 0;
    if (name[0] == '.' && pattern[0] != '.') return 0;
    for (;;) {
        size_t end = 0;
        if (pattern[p] == '*' && !protected_at(quoted, p)) {
            star = ++p;
            retry = n;
            continue;
        }
        if (pattern[p] == '\0' && name[n] == '\0') return 1;
        if (pattern[p] == '[' && !protected_at(quoted, p)) {
            end = bracket_end(pattern, quoted, p);
        }
        if (name[n] != '\0' && pattern[p] != '\0' &&
            ((end != 0 && bracket_match(pattern, quoted, p, end, (unsigned char)name[n])) ||
             (end == 0 && ((pattern[p] == '?' && !protected_at(quoted, p)) ||
                           pattern[p] == name[n])))) {
            p = end != 0 ? end : p + 1;
            n++;
            continue;
        }
        if (star != (size_t)-1 && name[retry] != '\0') {
            p = star;
            n = ++retry;
            continue;
        }
        return 0;
    }
}
