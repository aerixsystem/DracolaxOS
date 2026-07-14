/* kernel/linux/include-uapi/asm-generic/fcntl.h */
#ifndef _ASM_GENERIC_FCNTL_H
#define _ASM_GENERIC_FCNTL_H

/* open(2) flags */
#define O_ACCMODE       00000003
#define O_RDONLY        00000000
#define O_WRONLY        00000001
#define O_RDWR          00000002
#define O_CREAT         00000100
#define O_EXCL          00000200
#define O_NOCTTY        00000400
#define O_TRUNC         00001000
#define O_APPEND        00002000
#define O_NONBLOCK      00004000
#define O_DSYNC         00010000
#define O_DIRECT        00040000
#define O_LARGEFILE     00100000
#define O_DIRECTORY     00200000
#define O_NOFOLLOW      00400000
#define O_NOATIME       01000000
#define O_CLOEXEC       02000000

/* fcntl(2) commands */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4

/* fcntl file descriptor flags */
#define FD_CLOEXEC      1

/* access(2) mode bits */
#define F_OK            0
#define X_OK            1
#define W_OK            2
#define R_OK            4

#endif /* _ASM_GENERIC_FCNTL_H */
