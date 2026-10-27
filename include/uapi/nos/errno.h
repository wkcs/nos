/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS UAPI error codes.
 * Coexists cleanly with toolchain <errno.h> / <sys/errno.h> (Newlib, Picolibc, musl).
 */

#ifndef __NOS_UAPI_ERRNO_H__
#define __NOS_UAPI_ERRNO_H__

#ifndef __LINUX_ERRNO_EXTENSIONS__
#define __LINUX_ERRNO_EXTENSIONS__ 1
#endif

#if defined(__has_include) && __has_include(<errno.h>)
#include <errno.h>
#endif

#define NOS_OK                  0
#define NOS_EPERM               1
#define NOS_ENOENT              2
#define NOS_ESRCH               3
#define NOS_EINTR               4
#define NOS_EIO                 5
#define NOS_ENXIO               6
#define NOS_E2BIG               7
#define NOS_ENOEXEC             8
#define NOS_EBADF               9
#define NOS_ECHILD             10
#define NOS_EAGAIN             11
#define NOS_ENOMEM             12
#define NOS_EACCES             13
#define NOS_EFAULT             14
#define NOS_EBUSY              16
#define NOS_EEXIST             17
#define NOS_EXDEV              18
#define NOS_ENODEV             19
#define NOS_ENOTDIR            20
#define NOS_EISDIR             21
#define NOS_EINVAL             22
#define NOS_ENFILE             23
#define NOS_EMFILE             24
#define NOS_ENOTTY             25
#define NOS_EFBIG              27
#define NOS_ENOSPC             28
#define NOS_ESPIPE             29
#define NOS_EROFS              30
#define NOS_EPIPE              32
#define NOS_EDOM               33
#define NOS_ERANGE             34
#define NOS_ENOSYS             38
#define NOS_ENOTSUP            95
#define NOS_ETIMEDOUT         110

#ifndef EPERM
#define EPERM            1
#endif
#ifndef ENOENT
#define ENOENT           2
#endif
#ifndef ESRCH
#define ESRCH            3
#endif
#ifndef EINTR
#define EINTR            4
#endif
#ifndef EIO
#define EIO              5
#endif
#ifndef ENXIO
#define ENXIO            6
#endif
#ifndef E2BIG
#define E2BIG            7
#endif
#ifndef ENOEXEC
#define ENOEXEC          8
#endif
#ifndef EBADF
#define EBADF            9
#endif
#ifndef ECHILD
#define ECHILD          10
#endif
#ifndef EAGAIN
#define EAGAIN          11
#endif
#ifndef ENOMEM
#define ENOMEM          12
#endif
#ifndef EACCES
#define EACCES          13
#endif
#ifndef EFAULT
#define EFAULT          14
#endif
#ifndef ENOTBLK
#define ENOTBLK         15
#endif
#ifndef EBUSY
#define EBUSY           16
#endif
#ifndef EEXIST
#define EEXIST          17
#endif
#ifndef EXDEV
#define EXDEV           18
#endif
#ifndef ENODEV
#define ENODEV          19
#endif
#ifndef ENOTDIR
#define ENOTDIR         20
#endif
#ifndef EISDIR
#define EISDIR          21
#endif
#ifndef EINVAL
#define EINVAL          22
#endif
#ifndef ENFILE
#define ENFILE          23
#endif
#ifndef EMFILE
#define EMFILE          24
#endif
#ifndef ENOTTY
#define ENOTTY          25
#endif
#ifndef ETXTBSY
#define ETXTBSY         26
#endif
#ifndef EFBIG
#define EFBIG           27
#endif
#ifndef ENOSPC
#define ENOSPC          28
#endif
#ifndef ESPIPE
#define ESPIPE          29
#endif
#ifndef EROFS
#define EROFS           30
#endif
#ifndef EMLINK
#define EMLINK          31
#endif
#ifndef EPIPE
#define EPIPE           32
#endif
#ifndef EDOM
#define EDOM            33
#endif
#ifndef ERANGE
#define ERANGE          34
#endif
#ifndef EDEADLK
#define EDEADLK         35
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG    36
#endif
#ifndef ENOLCK
#define ENOLCK          37
#endif
#ifndef ENOSYS
#define ENOSYS          38
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY       39
#endif
#ifndef ELOOP
#define ELOOP           40
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK     EAGAIN
#endif
#ifndef ENOMSG
#define ENOMSG          42
#endif
#ifndef EIDRM
#define EIDRM           43
#endif
#ifndef ECHRNG
#define ECHRNG          44
#endif
#ifndef EL2NSYNC
#define EL2NSYNC        45
#endif
#ifndef EL3HLT
#define EL3HLT          46
#endif
#ifndef EL3RST
#define EL3RST          47
#endif
#ifndef ELNRNG
#define ELNRNG          48
#endif
#ifndef EUNATCH
#define EUNATCH         49
#endif
#ifndef ENOCSI
#define ENOCSI          50
#endif
#ifndef EL2HLT
#define EL2HLT          51
#endif
#ifndef EBADE
#define EBADE           52
#endif
#ifndef EBADR
#define EBADR           53
#endif
#ifndef EXFULL
#define EXFULL          54
#endif
#ifndef ENOANO
#define ENOANO          55
#endif
#ifndef EBADRQC
#define EBADRQC         56
#endif
#ifndef EBADSLT
#define EBADSLT         57
#endif
#ifndef EDEADLOCK
#define EDEADLOCK       EDEADLK
#endif
#ifndef EBFONT
#define EBFONT          59
#endif
#ifndef ENOSTR
#define ENOSTR          60
#endif
#ifndef ENODATA
#define ENODATA         61
#endif
#ifndef ETIME
#define ETIME           62
#endif
#ifndef ENOSR
#define ENOSR           63
#endif
#ifndef ENONET
#define ENONET          64
#endif
#ifndef ENOPKG
#define ENOPKG          65
#endif
#ifndef EREMOTE
#define EREMOTE         66
#endif
#ifndef ENOLINK
#define ENOLINK         67
#endif
#ifndef EADV
#define EADV            68
#endif
#ifndef ESRMNT
#define ESRMNT          69
#endif
#ifndef ECOMM
#define ECOMM           70
#endif
#ifndef EPROTO
#define EPROTO          71
#endif
#ifndef EMULTIHOP
#define EMULTIHOP       72
#endif
#ifndef EDOTDOT
#define EDOTDOT         73
#endif
#ifndef EBADMSG
#define EBADMSG         74
#endif
#ifndef EOVERFLOW
#define EOVERFLOW       75
#endif
#ifndef ENOTUNIQ
#define ENOTUNIQ        76
#endif
#ifndef EBADFD
#define EBADFD          77
#endif
#ifndef EREMCHG
#define EREMCHG         78
#endif
#ifndef ELIBACC
#define ELIBACC         79
#endif
#ifndef ELIBBAD
#define ELIBBAD         80
#endif
#ifndef ELIBSCN
#define ELIBSCN         81
#endif
#ifndef ELIBMAX
#define ELIBMAX         82
#endif
#ifndef ELIBEXEC
#define ELIBEXEC        83
#endif
#ifndef EILSEQ
#define EILSEQ          84
#endif
#ifndef ERESTART
#define ERESTART        150
#endif
#ifndef ESTRPIPE
#define ESTRPIPE        86
#endif
#ifndef EUSERS
#define EUSERS          87
#endif
#ifndef ENOTSOCK
#define ENOTSOCK        88
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ    89
#endif
#ifndef EMSGSIZE
#define EMSGSIZE        90
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE      91
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT     92
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 94
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP      95
#endif
#ifndef ENOTSUP
#define ENOTSUP         EOPNOTSUPP
#endif
#ifndef EPFNOSUPPORT
#define EPFNOSUPPORT    96
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT    97
#endif
#ifndef EADDRINUSE
#define EADDRINUSE      98
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL   99
#endif
#ifndef ENETDOWN
#define ENETDOWN        100
#endif
#ifndef ENETUNREACH
#define ENETUNREACH     101
#endif
#ifndef ENETRESET
#define ENETRESET       102
#endif
#ifndef ECONNABORTED
#define ECONNABORTED    103
#endif
#ifndef ECONNRESET
#define ECONNRESET      104
#endif
#ifndef ENOBUFS
#define ENOBUFS         105
#endif
#ifndef EISCONN
#define EISCONN         106
#endif
#ifndef ENOTCONN
#define ENOTCONN        107
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN       108
#endif
#ifndef ETOOMANYREFS
#define ETOOMANYREFS    109
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT       110
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED    111
#endif
#ifndef EHOSTDOWN
#define EHOSTDOWN       112
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH    113
#endif
#ifndef EALREADY
#define EALREADY        114
#endif
#ifndef EINPROGRESS
#define EINPROGRESS     115
#endif
#ifndef ESTALE
#define ESTALE          116
#endif
#ifndef EUCLEAN
#define EUCLEAN         151
#endif
#ifndef ENOTNAM
#define ENOTNAM         152
#endif
#ifndef ENAVAIL
#define ENAVAIL         153
#endif
#ifndef EISNAM
#define EISNAM          154
#endif
#ifndef EREMOTEIO
#define EREMOTEIO       155
#endif
#ifndef EDQUOT
#define EDQUOT          122
#endif
#ifndef ENOMEDIUM
#define ENOMEDIUM       123
#endif
#ifndef EMEDIUMTYPE
#define EMEDIUMTYPE     156
#endif
#ifndef ECANCELED
#define ECANCELED       125
#endif
#ifndef ENOKEY
#define ENOKEY          157
#endif
#ifndef EKEYEXPIRED
#define EKEYEXPIRED     158
#endif
#ifndef EKEYREVOKED
#define EKEYREVOKED     159
#endif
#ifndef EKEYREJECTED
#define EKEYREJECTED    160
#endif
#ifndef EOWNERDEAD
#define EOWNERDEAD      130
#endif
#ifndef ENOTRECOVERABLE
#define ENOTRECOVERABLE 131
#endif
#ifndef ERFKILL
#define ERFKILL         161
#endif
#ifndef EHWPOISON
#define EHWPOISON       162
#endif

#endif /* __NOS_UAPI_ERRNO_H__ */
