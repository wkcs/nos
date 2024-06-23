/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <string.h>
#include <kernel/errno.h>
#include <kernel/mm.h>

char *strcat(char *dest, const char *src)
{
	char *tmp = dest;

	while (*dest)
		dest++;
	while ((*dest++ = *src++) != '\0')
		;
	return tmp;
}

char *strncat(char *dest, const char *src, size_t count)
{
	char *tmp = dest;

	if (count) {
		while (*dest)
			dest++;
		while ((*dest++ = *src++) != 0) {
			if (--count == 0) {
				*dest = '\0';
				break;
			}
		}
	}
	return tmp;
}

size_t strlcat(char *dest, const char *src, size_t count)
{
	size_t dsize = strlen(dest);
	size_t len = strlen(src);
	size_t res = dsize + len;

	/* This would be a bug */
	BUG_ON(dsize >= count);

	dest += dsize;
	count -= dsize;
	if (len >= count)
		len = count-1;
	__builtin_memcpy(dest, src, len);
	dest[len] = 0;
	return res;
}

#ifndef __HAVE_ARCH_STRLEN
size_t strlen(const char *s)
{
    const char *sc;

    if (s == NULL)
        return 0;

    for (sc = s; *sc != '\0'; ++sc)
        /* nothing */;
    return sc - s;
}
#endif

#ifndef __HAVE_ARCH_MEMCPY
void *memcpy(void *dest, const void *src, size_t count)
{
    char *tmp = dest;
    const char *s = src;

    while (count--)
        *tmp++ = *s++;
    return dest;
}
#endif

#ifndef __HAVE_ARCH_MEMSET
void *memset(void *s, int c, size_t count)
{
    char *xs = s;

    while (count--)
        *xs++ = c;
    return s;
}
#endif

#ifndef __HAVE_ARCH_MEMMOVE
void *memmove(void *dest, const void *src, size_t count)
{
    char *tmp;
    const char *s;

    if (dest <= src) {
        tmp = dest;
        s = src;
        while (count--)
            *tmp++ = *s++;
    } else {
        tmp = dest;
        tmp += count;
        s = src;
        s += count;
        while (count--)
            *--tmp = *--s;
    }
    return dest;
}
#endif

#ifndef __HAVE_ARCH_MEMCMP
#undef memcmp
__visible int memcmp(const void *cs, const void *ct, size_t count)
{
    const unsigned char *su1, *su2;
    int res = 0;

    for (su1 = cs, su2 = ct; 0 < count; ++su1, ++su2, count--)
        if ((res = *su1 - *su2) != 0)
            break;
    return res;
}
#endif

#ifndef __HAVE_ARCH_MEMSCAN
void *memscan(void *addr, int c, size_t size)
{
    unsigned char *p = addr;

    while (size) {
        if (*p == c)
            return (void *)p;
        p++;
        size--;
    }
      return (void *)p;
}
#endif

#ifndef __HAVE_ARCH_STRSTR
char *strstr(const char *s1, const char *s2)
{
    size_t l1, l2;

    l2 = strlen(s2);
    if (!l2)
        return (char *)s1;
    l1 = strlen(s1);
    while (l1 >= l2) {
        l1--;
        if (!memcmp(s1, s2, l2))
            return (char *)s1;
        s1++;
    }
    return NULL;
}
#endif

#ifndef __HAVE_ARCH_STRNSTR
char *strnstr(const char *s1, const char *s2, size_t len)
{
    size_t l2;

    l2 = strlen(s2);
    if (!l2)
        return (char *)s1;
    while (len >= l2) {
        len--;
        if (!memcmp(s1, s2, l2))
            return (char *)s1;
        s1++;
    }
    return NULL;
}
#endif

#ifndef __HAVE_ARCH_MEMCHR
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n-- != 0) {
            if ((unsigned char)c == *p++) {
            return (void *)(p - 1);
        }
    }
    return NULL;
}
#endif

static void *check_bytes8(const u8 *start, u8 value, unsigned int bytes)
{
    while (bytes) {
        if (*start != value)
            return (void *)start;
        start++;
        bytes--;
    }
    return NULL;
}

void *memchr_inv(const void *start, int c, size_t bytes)
{
    u8 value = c;
    u64 value64;
    unsigned int words, prefix;

    if (bytes <= 16)
        return check_bytes8(start, value, bytes);

    value64 = value;
#if defined(CONFIG_ARCH_HAS_FAST_MULTIPLIER) && BITS_PER_LONG == 64
    value64 *= 0x0101010101010101ULL;
#elif defined(CONFIG_ARCH_HAS_FAST_MULTIPLIER)
    value64 *= 0x01010101;
    value64 |= value64 << 32;
#else
    value64 |= value64 << 8;
    value64 |= value64 << 16;
    value64 |= value64 << 32;
#endif

    prefix = (unsigned long)start % 8;
    if (prefix) {
        u8 *r;

        prefix = 8 - prefix;
        r = check_bytes8(start, value, prefix);
        if (r)
            return r;
        start += prefix;
        bytes -= prefix;
    }

    words = bytes / 8;

    while (words) {
        if (*(u64 *)start != value64)
            return check_bytes8(start, value, 8);
        start += 8;
        words--;
    }

    return check_bytes8(start, value, bytes % 8);
}

char *strreplace(char *s, char old, char new)
{
    for (; *s; ++s)
        if (*s == old)
            *s = new;
    return s;
}

int strncmp(const char *cs, const char *ct, size_t count)
{
    register signed char __res = 0;

    while (count)
    {
        if ((__res = *cs - *ct++) != 0 || !*cs++)
            break;
        count --;
    }

    return __res;
}

int strcmp(const char *cs, const char *ct)
{
    while (*cs && *cs == *ct)
        cs++, ct++;

    return (*cs - *ct);
}

char *strchr(const char *s, int c)
{
    for (; *s != (char)c; ++s)
        if (*s == '\0')
            return NULL;
    return (char *)s;
}

char *strpbrk(const char *cs, const char *ct)
{
    const char *sc;

    for (sc = cs; *sc != '\0'; ++sc) {
        if (strchr(ct, *sc))
            return (char *)sc;
    }
    return NULL;
}

char *strcpy(char *dest, const char *src)
{
    char *tmp = dest;

    while ((*dest++ = *src++) != '\0')
        /* nothing */;
    return tmp;
}

char *strncpy(char *dest, const char *src, size_t count)
{
    char *tmp = dest;

    while (count) {
        if ((*tmp = *src) != 0)
            src++;
        tmp++;
        count--;
    }
    return dest;
}

size_t strspn(const char *s, const char *accept)
{
    const char *p;

    for (p = s; *p != '\0'; ++p) {
        if (!strchr(accept, *p))
            break;
    }
    return p - s;
}

size_t strcspn(const char *s, const char *reject)
{
    const char *p;

    for (p = s; *p != '\0'; ++p) {
        if (strchr(reject, *p))
            break;
    }
    return p - s;
}

size_t strnlen(const char *s, size_t count)
{
    const char *sc;

    for (sc = s; count-- && *sc != '\0'; ++sc)
        /* nothing */;
    return sc - s;
}

int strcoll (const char *a, const char *b)

{
    return strcmp (a, b);
}

char *kstrdup(const char *s, gfp_t gfp)
{
	size_t len;
	char *buf;

	if (!s)
		return NULL;

	len = strlen(s) + 1;
	buf = kmalloc(len, gfp);
	if (buf)
		memcpy(buf, s, len);
	return buf;
}

char *strerror(int errnum)
{
  char *error;

  switch (errnum)
    {
    case 0:
      error = "Success";
      break;
/* go32 defines EPERM as EACCES */
#if defined (EPERM) && (!defined (EACCES) || (EPERM != EACCES))
    case EPERM:
      error = "Not owner";
      break;
#endif
#ifdef ENOENT
    case ENOENT:
      error = "No such file or directory";
      break;
#endif
#ifdef ESRCH
    case ESRCH:
      error = "No such process";
      break;
#endif
#ifdef EINTR
    case EINTR:
      error = "Interrupted system call";
      break;
#endif
#ifdef EIO
    case EIO:
      error = "I/O error";
      break;
#endif
/* go32 defines ENXIO as ENODEV */
#if defined (ENXIO) && (!defined (ENODEV) || (ENXIO != ENODEV))
    case ENXIO:
      error = "No such device or address";
      break;
#endif
#ifdef E2BIG
    case E2BIG:
      error = "Arg list too long";
      break;
#endif
#ifdef ENOEXEC
    case ENOEXEC:
      error = "Exec format error";
      break;
#endif
#ifdef EALREADY
    case EALREADY:
      error = "Socket already connected";
      break;
#endif
#ifdef EBADF
    case EBADF:
      error = "Bad file number";
      break;
#endif
#ifdef ECHILD
    case ECHILD:
      error = "No children";
      break;
#endif
#ifdef EDESTADDRREQ
    case EDESTADDRREQ:
      error = "Destination address required";
      break;
#endif
#ifdef EAGAIN
    case EAGAIN:
      error = "No more processes";
      break;
#endif
#ifdef ENOMEM
    case ENOMEM:
      error = "Not enough space";
      break;
#endif
#ifdef EACCES
    case EACCES:
      error = "Permission denied";
      break;
#endif
#ifdef EFAULT
    case EFAULT:
      error = "Bad address";
      break;
#endif
#ifdef ENOTBLK
    case ENOTBLK:
      error = "Block device required";
      break;
#endif
#ifdef EBUSY
    case EBUSY:
      error = "Device or resource busy";
      break;
#endif
#ifdef EEXIST
    case EEXIST:
      error = "File exists";
      break;
#endif
#ifdef EXDEV
    case EXDEV:
      error = "Cross-device link";
      break;
#endif
#ifdef ENODEV
    case ENODEV:
      error = "No such device";
      break;
#endif
#ifdef ENOTDIR
    case ENOTDIR:
      error = "Not a directory";
      break;
#endif
#ifdef EHOSTDOWN
    case EHOSTDOWN:
      error = "Host is down";
      break;
#endif
#ifdef EINPROGRESS
    case EINPROGRESS:
      error = "Connection already in progress";
      break;
#endif
#ifdef EISDIR
    case EISDIR:
      error = "Is a directory";
      break;
#endif
#ifdef EINVAL
    case EINVAL:
      error = "Invalid argument";
      break;
#endif
#ifdef ENETDOWN
    case ENETDOWN:
      error = "Network interface is not configured";
      break;
#endif
#ifdef ENETRESET
    case ENETRESET:
      error = "Connection aborted by network";
      break;
#endif
#ifdef ENFILE
    case ENFILE:
      error = "Too many open files in system";
      break;
#endif
#ifdef EMFILE
    case EMFILE:
      error = "File descriptor value too large";
      break;
#endif
#ifdef ENOTTY
    case ENOTTY:
      error = "Not a character device";
      break;
#endif
#ifdef ETXTBSY
    case ETXTBSY:
      error = "Text file busy";
      break;
#endif
#ifdef EFBIG
    case EFBIG:
      error = "File too large";
      break;
#endif
#ifdef EHOSTUNREACH
    case EHOSTUNREACH:
      error = "Host is unreachable";
      break;
#endif
#ifdef ENOSPC
    case ENOSPC:
      error = "No space left on device";
      break;
#endif
#ifdef ENOTSUP
    case ENOTSUP:
      error = "Not supported";
      break;
#endif
#ifdef ESPIPE
    case ESPIPE:
      error = "Illegal seek";
      break;
#endif
#ifdef EROFS
    case EROFS:
      error = "Read-only file system";
      break;
#endif
#ifdef EMLINK
    case EMLINK:
      error = "Too many links";
      break;
#endif
#ifdef EPIPE
    case EPIPE:
      error = "Broken pipe";
      break;
#endif
#ifdef EDOM
    case EDOM:
      error = "Mathematics argument out of domain of function";
      break;
#endif
#ifdef ERANGE
    case ERANGE:
      error = "Result too large";
      break;
#endif
#ifdef ENOMSG
    case ENOMSG:
      error = "No message of desired type";
      break;
#endif
#ifdef EIDRM
    case EIDRM:
      error = "Identifier removed";
      break;
#endif
#ifdef EILSEQ
    case EILSEQ:
      error = "Illegal byte sequence";
      break;
#endif
#ifdef EDEADLK
    case EDEADLK:
      error = "Deadlock";
      break;
#endif
#ifdef ENETUNREACH
    case  ENETUNREACH:
      error = "Network is unreachable";
      break;
#endif
#ifdef ENOLCK
    case ENOLCK:
      error = "No lock";
      break;
#endif
#ifdef ENOSTR
    case ENOSTR:
      error = "Not a stream";
      break;
#endif
#ifdef ETIME
    case ETIME:
      error = "Stream ioctl timeout";
      break;
#endif
#ifdef ENOSR
    case ENOSR:
      error = "No stream resources";
      break;
#endif
#ifdef ENONET
    case ENONET:
      error = "Machine is not on the network";
      break;
#endif
#ifdef ENOPKG
    case ENOPKG:
      error = "No package";
      break;
#endif
#ifdef EREMOTE
    case EREMOTE:
      error = "Resource is remote";
      break;
#endif
#ifdef ENOLINK
    case ENOLINK:
      error = "Virtual circuit is gone";
      break;
#endif
#ifdef EADV
    case EADV:
      error = "Advertise error";
      break;
#endif
#ifdef ESRMNT
    case ESRMNT:
      error = "Srmount error";
      break;
#endif
#ifdef ECOMM
    case ECOMM:
      error = "Communication error";
      break;
#endif
#ifdef EPROTO
    case EPROTO:
      error = "Protocol error";
      break;
#endif
#ifdef EPROTONOSUPPORT
    case EPROTONOSUPPORT:
      error = "Unknown protocol";
      break;
#endif
#ifdef EMULTIHOP
    case EMULTIHOP:
      error = "Multihop attempted";
      break;
#endif
#ifdef EBADMSG
    case EBADMSG:
      error = "Bad message";
      break;
#endif
#ifdef ELIBACC
    case ELIBACC:
      error = "Cannot access a needed shared library";
      break;
#endif
#ifdef ELIBBAD
    case ELIBBAD:
      error = "Accessing a corrupted shared library";
      break;
#endif
#ifdef ELIBSCN
    case ELIBSCN:
      error = ".lib section in a.out corrupted";
      break;
#endif
#ifdef ELIBMAX
    case ELIBMAX:
      error = "Attempting to link in more shared libraries than system limit";
      break;
#endif
#ifdef ELIBEXEC
    case ELIBEXEC:
      error = "Cannot exec a shared library directly";
      break;
#endif
#ifdef ENOSYS
    case ENOSYS:
      error = "Function not implemented";
      break;
#endif
#ifdef ENMFILE
    case ENMFILE:
      error = "No more files";
      break;
#endif
#ifdef ENOTEMPTY
    case ENOTEMPTY:
      error = "Directory not empty";
      break;
#endif
#ifdef ENAMETOOLONG
    case ENAMETOOLONG:
      error = "File or path name too long";
      break;
#endif
#ifdef ELOOP
    case ELOOP:
      error = "Too many symbolic links";
      break;
#endif
#ifdef ENOBUFS
    case ENOBUFS:
      error = "No buffer space available";
      break;
#endif
#ifdef ENODATA
    case ENODATA:
      error = "No data";
      break;
#endif
#ifdef EAFNOSUPPORT
    case EAFNOSUPPORT:
      error = "Address family not supported by protocol family";
      break;
#endif
#ifdef EPROTOTYPE
    case EPROTOTYPE:
      error = "Protocol wrong type for socket";
      break;
#endif
#ifdef ENOTSOCK
    case ENOTSOCK:
      error = "Socket operation on non-socket";
      break;
#endif
#ifdef ENOPROTOOPT
    case ENOPROTOOPT:
      error = "Protocol not available";
      break;
#endif
#ifdef ESHUTDOWN
    case ESHUTDOWN:
      error = "Can't send after socket shutdown";
      break;
#endif
#ifdef ECONNREFUSED
    case ECONNREFUSED:
      error = "Connection refused";
      break;
#endif
#ifdef ECONNRESET
    case ECONNRESET:
      error = "Connection reset by peer";
      break;
#endif
#ifdef EADDRINUSE
    case EADDRINUSE:
      error = "Address already in use";
      break;
#endif
#ifdef EADDRNOTAVAIL
    case EADDRNOTAVAIL:
      error = "Address not available";
      break;
#endif
#ifdef ECONNABORTED
    case ECONNABORTED:
      error = "Software caused connection abort";
      break;
#endif
#if (defined(EWOULDBLOCK) && (!defined (EAGAIN) || (EWOULDBLOCK != EAGAIN)))
    case EWOULDBLOCK:
        error = "Operation would block";
        break;
#endif
#ifdef ENOTCONN
    case ENOTCONN:
        error = "Socket is not connected";
        break;
#endif
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT:
        error = "Socket type not supported";
        break;
#endif
#ifdef EISCONN
    case EISCONN:
        error = "Socket is already connected";
        break;
#endif
#ifdef ECANCELED
    case ECANCELED:
        error = "Operation canceled";
        break;
#endif
#ifdef ENOTRECOVERABLE
    case ENOTRECOVERABLE:
        error = "State not recoverable";
        break;
#endif
#ifdef EOWNERDEAD
    case EOWNERDEAD:
        error = "Previous owner died";
        break;
#endif
#ifdef ESTRPIPE
    case ESTRPIPE:
	error = "Streams pipe error";
	break;
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || (ENOTSUP != EOPNOTSUPP))
    case EOPNOTSUPP:
        error = "Operation not supported on socket";
        break;
#endif
#ifdef EOVERFLOW
    case EOVERFLOW:
      error = "Value too large for defined data type";
      break;
#endif
#ifdef EMSGSIZE
    case EMSGSIZE:
        error = "Message too long";
        break;
#endif
#ifdef ETIMEDOUT
    case ETIMEDOUT:
        error = "Connection timed out";
        break;
#endif
    default:
      error = "Unknown";
      break;
    }

  return error;
}
