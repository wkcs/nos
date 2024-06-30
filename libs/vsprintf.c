/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <string.h>
#include <stdio.h>
#include <lib/vsprintf.h>

#define isdigit(c) ((unsigned)((c) - '0') < 10)

inline int divide(int *n, int base)
{
    int res;

    /* optimized for processor which does not support divide instructions. */
    if (base == 10) {
        res = ((uint32_t) * n) % 10U;
        *n = ((uint32_t) * n) / 10U;
    } else {
        res = ((uint32_t) * n) % 16U;
        *n = ((uint32_t) * n) / 16U;
    }

    return res;
}

inline int skip_atoi(const char **s)
{
    register int i = 0;

    while (isdigit(**s))
        i = i * 10 + *((*s)++) - '0';

    return i;
}

#define ZEROPAD (1 << 0)
#define SIGN (1 << 1)
#define PLUS (1 << 2)
#define SPACE (1 << 3)
#define LEFT (1 << 4)
#define SPECIAL (1 << 5)
#define LARGE (1 << 6)
#define FLOAT (1 << 7)

static char *print_number(char *buf, char *end, int num, int base, int s, int type)
{
    char c, sign;
    char tmp[16];
    const char *digits;
    static const char small_digits[] = "0123456789abcdef";
    static const char large_digits[] = "0123456789ABCDEF";
    register int i;
    register int size;

    size = s;

    digits = (type & LARGE) ? large_digits : small_digits;
    if (type & LEFT)
        type &= ~ZEROPAD;

    c = (type & ZEROPAD) ? '0' : ' ';

    /* get sign */
    sign = 0;
    if (type & SIGN) {
        if (num < 0) {
            sign = '-';
            num = -num;
        } else if (type & PLUS)
            sign = '+';
        else if (type & SPACE)
            sign = ' ';
    }
    i = 0;
    if (num == 0)
        tmp[i++] = '0';
    else {
        while (num != 0)
            tmp[i++] = digits[divide(&num, base)];
    }

    size -= i;

    if (!(type & (ZEROPAD | LEFT))) {
        if ((sign) && (size > 0))
            size--;
        while (size-- > 0) {
            if ((buf <= end) && (end != NULL))
                *buf = ' ';
            ++buf;
        }
    }
    if (sign) {
        if ((buf <= end) && (end != NULL)) {
            *buf = sign;
            --size;
        }
        ++buf;
    }

    /* no align to the left */
    if (!(type & LEFT)) {
        while (size-- > 0) {
            if ((buf <= end) && (end != NULL))
                *buf = c;
            ++buf;
        }
    }

    /* put number in the temporary buffer */
    while (i-- > 0) {
        if ((buf <= end) && (end != NULL))
            *buf = tmp[i];
        ++buf;
    }

    while (size-- > 0) {
        if ((buf <= end) && (end != NULL))
            *buf = ' ';
        ++buf;
    }

    return buf;
}

static char *print_float(char *buf, char *end, double num, int s, int type)
{
    char c, sign;
    char tmp[32];
    const char digits[] = "0123456789.";
    register int i, m;
    register int size;
    int integer = (int)num;
    double decimal = num - (int)num;

    size = s;

    c = (type & ZEROPAD) ? '0' : ' ';

    i = 0;
    if (integer == 0)
        tmp[7 + i++] = '0';
    else {
        while (integer != 0)
            tmp[7 + i++] = digits[divide(&integer, 10)];
    }
    if (type & LEFT)
        size = 0;
    else
        size -= i;
    tmp[6] = '.';
    i++;
    for (m = 0; m < 6; m++) {
        decimal *= 10;
        tmp[5 - m] = digits[(int)decimal];
        i++;
        decimal -= (int)decimal;
    }

    /* get sign */
    sign = 0;
    if (type & SIGN) {
        if (num < 0)
            sign = '-';
        else if (type & PLUS)
            sign = '+';
        else if (type & SPACE)
            sign = ' ';
    }

    if (!(type & (ZEROPAD | LEFT))) {
        if ((sign) && (size > 0))
            size--;
        while (size-- > 0) {
            if ((buf <= end) && (end != NULL))
                *buf = ' ';
            ++buf;
        }
    }
    if (sign) {
        if ((buf <= end) && (end != NULL)) {
            *buf = sign;
            --size;
        }
        ++buf;
    }

    /* no align to the left */
    if (!(type & LEFT)) {
        while (size-- > 0) {
            if ((buf <= end) && (end != NULL))
                *buf = c;
            ++buf;
        }
    }

    /* put number in the temporary buffer */
    while (i-- > 0) {
        if ((buf <= end) && (end != NULL))
            *buf = tmp[i];
        ++buf;
    }

    return buf;
}

__printf(3, 0) uint32_t vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    uint32_t num;
    double num_f;
    int i, len;
    char *str, *end, c;
    const char *s;

    uint8_t base;        /* the base of number */
    uint8_t flags;       /* flags to print number */
    uint8_t qualifier;   /* 'h', 'l', or 'L' for integer fields */
    int32_t field_width; /* width of output field */

    str = buf;
    if (buf == NULL)
        end = NULL;
    else
        end = buf + size - 1;

    /* Make sure end is always >= buf */
    if (end < buf) {
        end = ((char *) -1);
        size = end - buf;
    }

    for (; *fmt; ++fmt) {
        if (*fmt != '%') {
            if ((str <= end) && (buf != NULL))
                *str = *fmt;
            ++str;
            continue;
        }

        /* process flags */
        flags = 0;

        while (1) {
            /* skips the first '%' also */
            ++fmt;
            if (*fmt == '-')
                flags |= LEFT;
            else if (*fmt == '+')
                flags |= PLUS;
            else if (*fmt == ' ')
                flags |= SPACE;
            else if (*fmt == '#')
                flags |= SPECIAL;
            else if (*fmt == '0')
                flags |= ZEROPAD;
            else
                break;
        }

        /* get field width */
        field_width = -1;
        if (isdigit(*fmt))
            field_width = skip_atoi(&fmt);
        else if (*fmt == '*') {
            ++fmt;
            /* it's the next argument */
            field_width = va_arg(args, int);
            if (field_width < 0) {
                field_width = -field_width;
                flags |= LEFT;
            }
        }
        /* get the conversion qualifier */
        qualifier = 0;
        if (*fmt == 'h' || *fmt == 'l') {
            qualifier = *fmt;
            ++fmt;
        }

        /* the default base */
        base = 10;

        switch (*fmt) {
        case 'c':
            if (!(flags & LEFT)) {
                while (--field_width > 0) {
                    if ((str <= end) && (buf != NULL))
                        *str = ' ';
                    ++str;
                }
            }

            /* get character */
            c = (uint32_t)va_arg(args, int);
            if ((str <= end) && (buf != NULL))
                *str = c;
            ++str;

            /* put width */
            while (--field_width > 0) {
                if ((str <= end) && (buf != NULL))
                    *str = ' ';
                ++str;
            }
            continue;

        case 's':
            s = va_arg(args, char *);
            if (!s)
                s = "(NULL)";

            len = strlen(s);

            if (!(flags & LEFT)) {
                while (len < field_width--) {
                    if ((str <= end) && (buf != NULL))
                        *str = ' ';
                    ++str;
                }
            }

            for (i = 0; i < len; ++i) {
                if ((str <= end) && (buf != NULL))
                    *str = *s;
                ++str;
                ++s;
            }

            while (len < field_width--) {
                if ((str <= end) && (buf != NULL))
                    *str = ' ';
                ++str;
            }
            continue;

        case 'p':
            if (field_width == -1) {
                field_width = sizeof(void *) << 1;
                flags |= ZEROPAD;
            }
            str = print_number(str, end,
                               (long)va_arg(args, void *),
                               16, field_width, flags);
            continue;

        case '%':
            if ((str <= end) && (buf != NULL))
                *str = '%';
            ++str;
            continue;

        /* integer number formats - set up the flags and "break" */
        case 'o':
            base = 8;
            break;

        case 'X':
            flags |= LARGE;
            base = 16;
            break;

        case 'x':
            base = 16;
            break;

        case 'd':
        case 'i':
            flags |= SIGN;
        case 'u':
            break;
        case 'f':
            flags |= FLOAT;
            break;

        default:
            if ((str <= end) && (buf != NULL))
                *str = '%';
            ++str;

            if (*fmt) {
                if ((str <= end) && (buf != NULL))
                    *str = *fmt;
                ++str;
            } else {
                --fmt;
            }
            continue;
        }

        if (flags & FLOAT) {
            num_f = va_arg(args, double);
            str = print_float(str, end, num_f, field_width, flags);
        } else {
            if (qualifier == 'l') {
                num = va_arg(args, uint32_t);
                if (flags & SIGN)
                    num = (int32_t)num;
            } else if (qualifier == 'h') {
                num = (uint16_t)va_arg(args, int32_t);
                if (flags & SIGN)
                    num = (int16_t)num;
            } else {
                num = va_arg(args, uint32_t);
                if (flags & SIGN)
                    num = (int32_t)num;
            }
            str = print_number(str, end, num, base, field_width, flags);
        }
    }

    if ((str <= end) && (buf != NULL))
        *str = '\0';
    else if (end != NULL)
        *end = '\0';

    /* the trailing null byte doesn't count towards the total
    * ++str;
    */
    return str - buf;
}

__printf(3, 4) uint32_t snprintf(char *buf, size_t size, const char *fmt, ...)
{
    uint32_t n;
    va_list args;

    va_start(args, fmt);
    n = vsnprintf(buf, size, fmt, args);
    va_end(args);

    return n;
}

__printf(2, 0) uint32_t vsprintf(char *buf, const char *format, va_list arg_ptr)
{
    return vsnprintf(buf, SIZE_MAX, format, arg_ptr);
}

__printf(2, 3) uint32_t sprintf(char *buf, const char *format, ...)
{
    uint32_t n;
    va_list arg_ptr;

    va_start(arg_ptr, format);
    n = vsprintf(buf, format, arg_ptr);
    va_end(arg_ptr);

    return n;
}
