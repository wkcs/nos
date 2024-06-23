/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __STDIO_H__
#define __STDIO_H__

#include <lib/vsprintf.h>
#include <kernel/errno.h>
#include <kernel/types.h>
#include <kernel/printk.h>

#define FILE int
#ifdef __BUFSIZ__
#define	BUFSIZ		__BUFSIZ__
#else
#define	BUFSIZ		1024
#endif

#ifdef __FOPEN_MAX__
#define FOPEN_MAX	__FOPEN_MAX__
#else
#define	FOPEN_MAX	20
#endif

#ifdef __FILENAME_MAX__
#define FILENAME_MAX    __FILENAME_MAX__
#else
#define	FILENAME_MAX	1024
#endif

#ifdef __L_tmpnam__
#define L_tmpnam	__L_tmpnam__
#else
#define	L_tmpnam	FILENAME_MAX
#endif

#define	_IOFBF	0		/* setvbuf should set fully buffered */
#define	_IOLBF	1		/* setvbuf should set line buffered */
#define	_IONBF	2		/* setvbuf should set unbuffered */

#define EOF (-1)
#define stdin ((FILE *)0)
#define stdout ((FILE *)1)
#define stderr ((FILE *)2)

#ifndef SEEK_SET
#define	SEEK_SET	0	/* set file offset to offset */
#endif
#ifndef SEEK_CUR
#define	SEEK_CUR	1	/* set file offset to current plus offset */
#endif
#ifndef SEEK_END
#define	SEEK_END	2	/* set file offset to EOF plus offset */
#endif

inline static int feof(FILE *stream)
{
    return EOF;
}

inline static size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    return 0;
}

inline static FILE *fopen(const char *filename, const char *mode)
{
    return stdout;
}

inline static int getc(FILE *stream)
{
    return 0;
}

inline static FILE *freopen(const char *filename, const char *mode, FILE *stream)
{
    return stdout;
}

inline static int ferror(FILE *stream)
{
    return 0;
}

inline static int fclose(FILE *stream)
{
    return 0;
}

#define fprintf(stream, format, ...) pr_info(format,  ## __VA_ARGS__)

inline static int fflush(FILE *stream)
{
    return 0;
}

inline static size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    return pr_info("%s", ptr);
}

inline static char *fgets(char *str, int n, FILE *stream)
{
    return NULL;
}

inline static FILE *tmpfile(void)
{
    return NULL;
}

inline static int ungetc(int chr, FILE *stream)
{
    return 0;
}

inline static void clearerr(FILE *stream)
{}

inline static int fseek(FILE *stream, long int offset, int whence)
{
    return 0;
}

inline static long int ftell(FILE *stream)
{
    return 0;
}

inline static int setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    return 0;
}

inline static int remove(const char *filename)
{
    return 0;
}

inline static int rename(const char *old_filename, const char *new_filename)
{
    return 0;
}

inline static char *tmpnam(char *str)
{
    return NULL;
}

#endif