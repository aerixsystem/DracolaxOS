/* kernel/shell.h */
#ifndef SHELL_H
#define SHELL_H

/* Pull in comp_term_t so shell_attach_terminal has the correct type.
 * shell.c includes compositor.h directly; the header guard prevents
 * double-inclusion. */
#include "../gui/compositor/compositor.h"

/* Run the interactive shell (blocking; call from init task) */
void shell_run(void);
void shell_exec_line(const char *line);  /* execute one shell command */

/* Terminal window hook: when set, all shell_print/shell_putchar output
 * is routed to the compositor window terminal instead of VGA/fb_console. */
void shell_attach_terminal(comp_term_t *t);
void shell_detach_terminal(void);

#endif /* SHELL_H */
