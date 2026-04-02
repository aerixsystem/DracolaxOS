/* kernel/log.h — kernel logging (VGA + serial) */
#ifndef LOG_H
#define LOG_H

/* Log levels */
#define LOG_INFO  0
#define LOG_WARN  1
#define LOG_ERROR 2
#define LOG_DEBUG 3

void log_init(void);

/* printk(level, fmt, ...) — prints to VGA and COM1 serial */
void printk(int level, const char *fmt, ...);

/* Convenience macros */
#define kinfo(...)  printk(LOG_INFO,  __VA_ARGS__)
#define kwarn(...)  printk(LOG_WARN,  __VA_ARGS__)
#define kerror(...) printk(LOG_ERROR, __VA_ARGS__)
#define kdebug(...) printk(LOG_DEBUG, __VA_ARGS__)

/* Kernel panic: print message and halt */
void kpanic(const char *msg) __attribute__((noreturn));

#endif /* LOG_H */
