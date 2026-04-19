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

/* Kernel panic: print message, dump state, halt CPU */
void kpanic(const char *msg) __attribute__((noreturn));

/* panic() — canonical name matching the stability spec.
 * Both panic() and kpanic() call the same implementation. */
#define panic(msg) kpanic(msg)

#endif /* LOG_H */
