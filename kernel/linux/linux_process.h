/* kernel/linux/linux_process.h */
#ifndef LINUX_PROCESS_H
#define LINUX_PROCESS_H

int lx_fork_task(void);
int lx_execve_impl(const char *path, char *const argv[], char *const envp[]);
int lx_waitpid_impl(int pid, int *wstatus, int options);

#endif /* LINUX_PROCESS_H */
