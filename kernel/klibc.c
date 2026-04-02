/* kernel/klibc.c */
#include "types.h"
#include "klibc.h"

/* ---- Memory -------------------------------------------------------------- */

void *memset(void *dst, int c, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

/* ---- Strings ------------------------------------------------------------- */

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}
char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    while (n > 0 && *src) { *d++ = *src++; n--; }
    *d = '\0';
    return dst;
}

/* strlcpy — BSD-safe strcpy: always NUL-terminates, returns src length.
 * Safer than strncpy for kernel buffer fills. */
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t srclen = strlen(src);
    if (size > 0) {
        size_t copy = (srclen < size - 1) ? srclen : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}

/* strlcat — BSD-safe strcat: always NUL-terminates, returns total length. */
size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);
    if (dlen >= size) return dlen + slen;   /* already full */
    size_t avail = size - dlen - 1;
    size_t copy  = (slen < avail) ? slen : avail;
    memcpy(dst + dlen, src, copy);
    dst[dlen + copy] = '\0';
    return dlen + slen;
}


char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    return (c == '\0') ? (char *)s : (char *)last;
}

/* ---- Number formatting --------------------------------------------------- */

/* 64-bit base conversion — handles all integer sizes on x86_64 */
void utoa64(uint64_t val, char *buf, int base) {
    static const char digits[] = "0123456789abcdef";
    char tmp[68];
    int  i = 66;   /* fill from end — no reverse pass needed */
    tmp[67] = '\0';
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (val) {
        tmp[--i] = digits[(int)(val % (uint64_t)base)];
        val /= (uint64_t)base;
    }
    strcpy(buf, &tmp[i]);   /* single pass copy from the filled region */
}

/* 32-bit wrapper kept for compatibility */
void utoa(unsigned val, char *buf, int base) {
    utoa64((uint64_t)val, buf, base);
}

void itoa(int val, char *buf, int base) {
    if (val < 0 && base == 10) {
        *buf++ = '-';
        utoa64((uint64_t)(-(int64_t)val), buf, base);
    } else {
        utoa64((uint64_t)(unsigned)val, buf, base);
    }
}

int atoi(const char *s) {
    int result = 0, sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') result = result * 10 + (*s++ - '0');
    return sign * result;
}

/* ---- snprintf ------------------------------------------------------------ */

static void fmt_string(char *buf, size_t *pos, size_t size, const char *s) {
    while (*s && *pos + 1 < size) buf[(*pos)++] = *s++;
}

/* All integer formatting uses uint64_t internally — no pointer-size issues */
static void fmt_uint(char *buf, size_t *pos, size_t size,
                     uint64_t val, int base, int pad, char padc) {
    char tmp[66];
    utoa64(val, tmp, base);
    int len = (int)strlen(tmp);
    while (pad > len && *pos + 1 < size) { buf[(*pos)++] = padc; pad--; }
    fmt_string(buf, pos, size, tmp);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    if (!size) return 0;
    size_t pos = 0;

    while (*fmt && pos + 1 < size) {
        if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
        fmt++; /* skip % */
        int pad = 0; char padc = ' ';
        if (*fmt == '0') { padc = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') pad = pad * 10 + (*fmt++ - '0');

        /* Handle %ll prefix for 64-bit integers */
        int is_ll = 0;
        if (*fmt == 'l' && *(fmt+1) == 'l') { is_ll = 1; fmt += 2; }
        else if (*fmt == 'l') { fmt++; } /* single %l treated as 64-bit too */

        switch (*fmt) {
        case 'd': {
            int64_t v = is_ll ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            if (v < 0) {
                /* Write '-' first, then pad the number within the remaining width.
                 * %05d of -5 → "-0005", not "-00005". */
                if (pos + 1 < size) buf[pos++] = '-';
                v = -v;
                if (pad > 0) pad--;   /* '-' consumes one width slot */
            }
            fmt_uint(buf, &pos, size, (uint64_t)v, 10, pad, padc); break; }
        case 'u':
            fmt_uint(buf, &pos, size,
                     is_ll ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned),
                     10, pad, padc); break;
        case 'x':
            fmt_uint(buf, &pos, size,
                     is_ll ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned),
                     16, pad, padc); break;
        case 'p': {
            if (pos + 2 < size) { buf[pos++]='0'; buf[pos++]='x'; }
            fmt_uint(buf, &pos, size, (uint64_t)(uintptr_t)va_arg(ap, void*),
                     16, 16, '0'); break; }
        case 's': fmt_string(buf, &pos, size, va_arg(ap, char*)); break;
        case 'c': if (pos + 1 < size) buf[pos++] = (char)va_arg(ap, int); break;
        case '%': if (pos + 1 < size) buf[pos++] = '%'; break;
        default:  if (pos + 1 < size) buf[pos++] = *fmt; break;
        }
        fmt++;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
