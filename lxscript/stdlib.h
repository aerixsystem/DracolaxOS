/* lxscript/stdlib.h — kernel-compat shim
 *
 * Provides strtoll for lxscript lexer (number literal parsing).
 * strtod removed — float literals stored as long long (integer truncation)
 * to avoid SSE register returns, which are forbidden with -mno-sse.
 */
#pragma once
#include "../kernel/types.h"
#include "ctype.h"

/* strtoll — parse signed 64-bit integer in given base (2..36).
 * Stops at first non-digit character. endptr set if non-NULL. */
static inline long long strtoll(const char *s, char **endptr, int base) {
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) { base=16; s+=2; }
        else if (s[0]=='0') { base=8; s++; }
        else base=10;
    }
    long long v = 0;
    while (*s) {
        int d;
        unsigned char c = (unsigned char)*s;
        if      (isdigit(c))        d = c - '0';
        else if (c>='a'&&c<='z')    d = c - 'a' + 10;
        else if (c>='A'&&c<='Z')    d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}

/* strtod removed — see lexer.c. Kept as a no-op fallback for safety. */
