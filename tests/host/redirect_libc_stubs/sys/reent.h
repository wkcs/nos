#ifndef TEST_SYS_REENT_H
#define TEST_SYS_REENT_H
#include <sys/types.h>
struct _reent {
    int _errno;
};
#ifndef _REENT
#define _REENT ((struct _reent *)0)
#endif
#ifndef _ssize_t
typedef ssize_t _ssize_t;
#endif
#ifndef _off_t
typedef off_t _off_t;
#endif
#endif
