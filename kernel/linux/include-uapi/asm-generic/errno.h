/* kernel/linux/include-uapi/asm-generic/errno.h */
#ifndef _ASM_GENERIC_ERRNO_H
#define _ASM_GENERIC_ERRNO_H

#include "errno-base.h"

#define EDEADLK        35
#define ENAMETOOLONG   36
#define ENOLCK         37
#define ENOSYS         38   /* Function not implemented       */
#define ENOTEMPTY      39
#define ELOOP          40
#define EWOULDBLOCK    EAGAIN
#define ENOMSG         42
#define EIDRM          43
#define ENOSTR         60
#define ENODATA        61
#define ETIME          62
#define ENOSR          63
#define EREMOTE        66
#define ENOLINK        67
#define EPROTO         71
#define EMULTIHOP      72
#define EBADMSG        74
#define EOVERFLOW      75
#define EILSEQ         84
#define EUSERS         87
#define ENOTSOCK       88
#define EDESTADDRREQ   89
#define EMSGSIZE       90
#define EPROTOTYPE     91
#define ENOPROTOOPT    92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP     95
#define EAFNOSUPPORT   97
#define EADDRINUSE     98
#define EADDRNOTAVAIL  99
#define ENETDOWN       100
#define ENETUNREACH    101
#define ENETRESET      102
#define ECONNABORTED   103
#define ECONNRESET     104
#define ENOBUFS        105
#define EISCONN        106
#define ENOTCONN       107
#define ESHUTDOWN      108
#define ETOOMANYREFS   109
#define ETIMEDOUT      110
#define ECONNREFUSED   111
#define EHOSTDOWN      112
#define EHOSTUNREACH   113
#define EALREADY       114
#define EINPROGRESS    115
#define ESTALE         116

#endif /* _ASM_GENERIC_ERRNO_H */
