#include <kernel/types.h>
#include <kernel/mm.h>
#include <string.h>

void _exit(int status)
{
}

int _close(int fd)
{
    return 0;
}

int _lseek(int fd, int offset, int whence)
{
    return 0;
}

int _read(int fd, char *buf, int nbytes)
{
    return 0;
}

int _kill(int pid, int sig)
{
    return 0;
}

int _getpid(void)
{
    return 1;
}

char *_sbrk (int nbytes)
{
    return NULL;
}

unsigned long _times(void * tp)
{
    return 0;
}

int _write (int fd, const char *ptr, size_t len)
{
    return 0;
}

int _fstat (int file, void *st)
{
    return 0;
}

int _isatty(int fd)
{
    return 0;
}

int _gettimeofday (void * tp, void * tzp)
{
    return 0;
}

void *malloc(size_t size)
{
    return kmalloc(size, GFP_KERNEL);
}

void *calloc(size_t nitems, size_t size)
{
    return kzalloc(size, GFP_KERNEL);
}

void *realloc(void *ptr, u32 size)
{
    return krealloc(ptr, size, GFP_KERNEL);
}

void free(void *ptr)
{
    kfree(ptr);
}
