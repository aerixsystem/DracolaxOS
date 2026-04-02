/* kernel/linux/linux_types.h
 * Aliases between Linux uapi types and DracolaxOS native types.
 */
#ifndef LINUX_TYPES_H
#define LINUX_TYPES_H

#include "../types.h"

typedef int32_t  lx_ssize_t;
typedef uint32_t lx_size_t;
typedef int32_t  lx_off_t;
typedef int32_t  lx_pid_t;
typedef uint32_t lx_uid_t;
typedef uint32_t lx_gid_t;
typedef uint32_t lx_mode_t;
typedef uint32_t lx_dev_t;
typedef uint32_t lx_ino_t;
typedef uint32_t lx_nlink_t;
typedef int32_t  lx_time_t;

/* stat structure (Linux i386 old stat) */
typedef struct lx_stat {
    lx_dev_t    st_dev;
    lx_ino_t    st_ino;
    lx_mode_t   st_mode;
    lx_nlink_t  st_nlink;
    lx_uid_t    st_uid;
    lx_gid_t    st_gid;
    lx_dev_t    st_rdev;
    lx_off_t    st_size;
    uint32_t    st_blksize;
    uint32_t    st_blocks;
    lx_time_t   st_atime;
    uint32_t    st_atime_nsec;
    lx_time_t   st_mtime;
    uint32_t    st_mtime_nsec;
    lx_time_t   st_ctime;
    uint32_t    st_ctime_nsec;
} lx_stat_t;

/* dirent64 */
typedef struct lx_dirent64 {
    uint64_t    d_ino;
    int64_t     d_off;
    uint16_t    d_reclen;
    uint8_t     d_type;
    char        d_name[256];
} lx_dirent64_t;

/* iovec */
typedef struct lx_iovec {
    void       *iov_base;
    lx_size_t   iov_len;
} lx_iovec_t;

/* timespec */
typedef struct lx_timespec {
    lx_time_t   tv_sec;
    uint32_t    tv_nsec;
} lx_timespec_t;

/* timeval */
typedef struct lx_timeval {
    lx_time_t   tv_sec;
    uint32_t    tv_usec;
} lx_timeval_t;

#endif /* LINUX_TYPES_H */
