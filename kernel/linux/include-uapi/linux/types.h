/* include/uapi/linux/types.h — from Linux v6.x */
#ifndef _UAPI_LINUX_TYPES_H
#define _UAPI_LINUX_TYPES_H

/* These are already defined by kernel/types.h; this stub exists so
 * copied uapi headers that include <linux/types.h> compile cleanly. */
#ifndef __KERNEL__
# include <stdint.h>
#endif

typedef uint8_t   __u8;
typedef uint16_t  __u16;
typedef uint32_t  __u32;
typedef uint64_t  __u64;
typedef int8_t    __s8;
typedef int16_t   __s16;
typedef int32_t   __s32;
typedef int64_t   __s64;
typedef __u16     __le16;
typedef __u32     __le32;
typedef __u64     __le64;
typedef __u16     __be16;
typedef __u32     __be32;
typedef __u64     __be64;
typedef __u16     __sum16;
typedef __u32     __wsum;

#endif /* _UAPI_LINUX_TYPES_H */
