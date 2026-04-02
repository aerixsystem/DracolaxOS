/* kernel/klibc.h — kernel-side C library substitutes */
#ifndef KLIBC_H
#include "types.h"
#define KLIBC_H


/* Memory */
void *memset (void *dst, int c, size_t n);
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* Strings */
size_t strlen (const char *s);
char  *strcpy (char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
int    strcmp (const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcat (char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);   /* BSD-safe: always NUL-terminates */
size_t strlcat(char *dst, const char *src, size_t size);   /* BSD-safe: always NUL-terminates */
char  *strchr (const char *s, int c);
char  *strrchr(const char *s, int c);

/* Number formatting */
void   itoa(int  val, char *buf, int base);   /* signed */
void   utoa(unsigned val, char *buf, int base); /* unsigned */
void   utoa64(uint64_t val, char *buf, int base); /* 64-bit */
int    atoi(const char *s);

/* Simple printf to a buffer */
int snprintf (char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

#endif /* KLIBC_H */
