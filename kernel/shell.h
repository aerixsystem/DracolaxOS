/* kernel/shell.h */
#ifndef SHELL_H
#define SHELL_H

/* Run the interactive shell (blocking; call from init task) */
void shell_run(void);
void shell_exec_line(const char *line);  /* execute one shell command */

#endif /* SHELL_H */
