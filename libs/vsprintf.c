/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Bounded kernel formatting routines.
 */

#include <kernel/kernel.h>
#include <lib/vsprintf.h>
#include <limits.h>

#define FORMAT_LEFT    (1U << 0)
#define FORMAT_PLUS    (1U << 1)
#define FORMAT_SPACE   (1U << 2)
#define FORMAT_ZERO    (1U << 3)
#define FORMAT_ALT     (1U << 4)
#define FORMAT_UPPER   (1U << 5)

struct format_output {
    char *buf;
    size_t size;
    size_t count;
};

enum format_length {
    FORMAT_DEFAULT,
    FORMAT_CHAR,
    FORMAT_SHORT,
    FORMAT_LONG,
    FORMAT_LONG_LONG,
    FORMAT_SIZE,
    FORMAT_PTRDIFF,
    FORMAT_INTMAX,
};

static void format_char(struct format_output *out, char ch)
{
    if (out->buf != NULL && out->size != 0 && out->count < out->size - 1) {
        out->buf[out->count] = ch;
    }
    if (out->count < SIZE_MAX) {
        out->count++;
    }
}

static void format_repeat(struct format_output *out, char ch, int count)
{
    while (count-- > 0) {
        format_char(out, ch);
    }
}

static int parse_number(const char **fmt)
{
    int value = 0;

    while (**fmt >= '0' && **fmt <= '9') {
        int digit = *(*fmt)++ - '0';
        if (value > (INT_MAX - digit) / 10) {
            value = INT_MAX;
        } else {
            value = value * 10 + digit;
        }
    }
    return value;
}

static void format_text(struct format_output *out, const char *text, size_t len,
                        int width, unsigned int flags)
{
    int padding = (width > 0 && (size_t)width > len) ? width - (int)len : 0;

    if (!(flags & FORMAT_LEFT)) {
        format_repeat(out, ' ', padding);
    }
    while (len-- > 0) {
        format_char(out, *text++);
    }
    if (flags & FORMAT_LEFT) {
        format_repeat(out, ' ', padding);
    }
}

static void format_integer(struct format_output *out, uint64_t value,
                           bool negative, unsigned int base, int width,
                           int precision, unsigned int flags)
{
    const char *digits = (flags & FORMAT_UPPER) ?
                         "0123456789ABCDEF" : "0123456789abcdef";
    char reversed[65];
    char sign = 0;
    char prefix[2];
    int prefix_len = 0;
    int count = 0;
    int zeroes;
    int padding;
    uint64_t original = value;

    if (negative) {
        sign = '-';
    } else if (flags & FORMAT_PLUS) {
        sign = '+';
    } else if (flags & FORMAT_SPACE) {
        sign = ' ';
    }

    if (value != 0 || precision != 0) {
        do {
            reversed[count++] = digits[value % base];
            value /= base;
        } while (value != 0);
    }
    if ((flags & FORMAT_ALT) && base == 16 && original != 0) {
        prefix[0] = '0';
        prefix[1] = (flags & FORMAT_UPPER) ? 'X' : 'x';
        prefix_len = 2;
    } else if ((flags & FORMAT_ALT) && base == 8 &&
               (count == 0 || (original != 0 && precision <= count))) {
        prefix[0] = '0';
        prefix_len = 1;
    }

    zeroes = precision > count ? precision - count : 0;
    {
        int64_t remaining = (int64_t)width - count - zeroes - prefix_len - (sign != 0);
        padding = remaining > 0 ? (int)remaining : 0;
    }
    if (!(flags & FORMAT_LEFT) && !(flags & FORMAT_ZERO && precision < 0)) {
        format_repeat(out, ' ', padding);
    }
    if (sign != 0) {
        format_char(out, sign);
    }
    for (int i = 0; i < prefix_len; i++) {
        format_char(out, prefix[i]);
    }
    if (!(flags & FORMAT_LEFT) && (flags & FORMAT_ZERO) && precision < 0) {
        format_repeat(out, '0', padding);
    }
    format_repeat(out, '0', zeroes);
    while (count > 0) {
        format_char(out, reversed[--count]);
    }
    if (flags & FORMAT_LEFT) {
        format_repeat(out, ' ', padding);
    }
}

static uint64_t format_unsigned_arg(va_list *args, enum format_length length)
{
    switch (length) {
    case FORMAT_CHAR:
        return (unsigned char)va_arg(*args, unsigned int);
    case FORMAT_SHORT:
        return (unsigned short)va_arg(*args, unsigned int);
    case FORMAT_LONG:
        return va_arg(*args, unsigned long);
    case FORMAT_LONG_LONG:
        return va_arg(*args, unsigned long long);
    case FORMAT_SIZE:
        return va_arg(*args, size_t);
    case FORMAT_PTRDIFF:
        return (uint64_t)va_arg(*args, ptrdiff_t);
    case FORMAT_INTMAX:
        return va_arg(*args, uintmax_t);
    default:
        return va_arg(*args, unsigned int);
    }
}

static int64_t format_signed_arg(va_list *args, enum format_length length)
{
    switch (length) {
    case FORMAT_CHAR:
        return (signed char)va_arg(*args, int);
    case FORMAT_SHORT:
        return (short)va_arg(*args, int);
    case FORMAT_LONG:
        return va_arg(*args, long);
    case FORMAT_LONG_LONG:
        return va_arg(*args, long long);
    case FORMAT_SIZE:
    case FORMAT_PTRDIFF:
        return va_arg(*args, ptrdiff_t);
    case FORMAT_INTMAX:
        return va_arg(*args, intmax_t);
    default:
        return va_arg(*args, int);
    }
}

/* Keep the embedded formatter's fixed-point %f support without allocating. */
static void format_float(struct format_output *out, double number, int width,
                         int precision, unsigned int flags)
{
    char reversed[32];
    int digits = 0;
    int padding;
    char sign = 0;
    bool negative = __builtin_signbit(number);
    uint64_t integer;
    double fraction;
    double rounding = 0.5;

    if (__builtin_isnan(number)) {
        format_text(out, "nan", 3, width, flags);
        return;
    }
    if (__builtin_isinf(number) || number >= 18446744073709551616.0 ||
        number <= -18446744073709551616.0) {
        format_text(out, negative ? "-inf" : "inf", negative ? 4 : 3, width, flags);
        return;
    }
    if (negative) {
        number = -number;
        sign = '-';
    } else if (flags & FORMAT_PLUS) {
        sign = '+';
    } else if (flags & FORMAT_SPACE) {
        sign = ' ';
    }
    if (precision < 0) {
        precision = 6;
    }
    for (int i = 0; i < precision && rounding != 0.0; i++) {
        rounding /= 10.0;
    }
    number += rounding;
    integer = (uint64_t)number;
    fraction = number - (double)integer;
    do {
        reversed[digits++] = (char)('0' + integer % 10);
        integer /= 10;
    } while (integer != 0);
    {
        int64_t remaining = (int64_t)width - digits - precision - (sign != 0) -
                            (precision != 0 || (flags & FORMAT_ALT));
        padding = remaining > 0 ? (int)remaining : 0;
    }
    if (!(flags & FORMAT_LEFT) && !(flags & FORMAT_ZERO)) {
        format_repeat(out, ' ', padding);
    }
    if (sign != 0) {
        format_char(out, sign);
    }
    if (!(flags & FORMAT_LEFT) && (flags & FORMAT_ZERO)) {
        format_repeat(out, '0', padding);
    }
    while (digits > 0) {
        format_char(out, reversed[--digits]);
    }
    if (precision != 0 || (flags & FORMAT_ALT)) {
        format_char(out, '.');
    }
    while (precision-- > 0) {
        int digit;
        fraction *= 10;
        digit = (int)fraction;
        format_char(out, (char)('0' + digit));
        fraction -= digit;
    }
    if (flags & FORMAT_LEFT) {
        format_repeat(out, ' ', padding);
    }
}

__printf(3, 0) int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    struct format_output out = { .buf = buf, .size = size, .count = 0 };
    va_list ap;

    va_copy(ap, args);
    while (*fmt != '\0') {
        unsigned int flags = 0;
        unsigned int base = 10;
        enum format_length length = FORMAT_DEFAULT;
        int width = 0;
        int precision = -1;
        char spec;

        if (*fmt != '%') {
            format_char(&out, *fmt++);
            continue;
        }
        fmt++;
        for (;;) {
            if (*fmt == '-') flags |= FORMAT_LEFT;
            else if (*fmt == '+') flags |= FORMAT_PLUS;
            else if (*fmt == ' ') flags |= FORMAT_SPACE;
            else if (*fmt == '0') flags |= FORMAT_ZERO;
            else if (*fmt == '#') flags |= FORMAT_ALT;
            else break;
            fmt++;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
            if (width < 0) {
                flags |= FORMAT_LEFT;
                width = width == INT_MIN ? INT_MAX : -width;
            }
        } else {
            width = parse_number(&fmt);
        }
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
                if (precision < 0) precision = -1;
            } else {
                precision = parse_number(&fmt);
            }
        }
        if (*fmt == 'h') {
            fmt++;
            length = *fmt == 'h' ? FORMAT_CHAR : FORMAT_SHORT;
            if (*fmt == 'h') fmt++;
        } else if (*fmt == 'l') {
            fmt++;
            length = *fmt == 'l' ? FORMAT_LONG_LONG : FORMAT_LONG;
            if (*fmt == 'l') fmt++;
        } else if (*fmt == 'z') {
            length = FORMAT_SIZE;
            fmt++;
        } else if (*fmt == 't') {
            length = FORMAT_PTRDIFF;
            fmt++;
        } else if (*fmt == 'j') {
            length = FORMAT_INTMAX;
            fmt++;
        }
        spec = *fmt;
        if (spec == '\0') {
            format_char(&out, '%');
            break;
        }
        fmt++;
        switch (spec) {
        case 'c': {
            char ch = (char)va_arg(ap, int);
            format_text(&out, &ch, 1, width, flags);
            break;
        }
        case 's': {
            const char *text = va_arg(ap, const char *);
            size_t len = 0;
            if (text == NULL) text = "(NULL)";
            while ((precision < 0 || len < (size_t)precision) && text[len] != '\0') {
                len++;
            }
            format_text(&out, text, len, width, flags);
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(ap, void *);
            if (width == 0) {
                width = (int)(sizeof(void *) * 2);
                flags |= FORMAT_ZERO;
            }
            format_integer(&out, value, false, 16, width, precision, flags);
            break;
        }
        case 'd':
        case 'i': {
            int64_t value = format_signed_arg(&ap, length);
            bool negative = value < 0;
            uint64_t magnitude = negative ? 0U - (uint64_t)value : (uint64_t)value;
            format_integer(&out, magnitude, negative, 10, width, precision, flags);
            break;
        }
        case 'X':
            flags |= FORMAT_UPPER;
            /* fall through */
        case 'x':
            base = 16;
            /* fall through */
        case 'o':
            if (spec == 'o') base = 8;
            /* fall through */
        case 'u':
            flags &= ~(FORMAT_PLUS | FORMAT_SPACE);
            format_integer(&out, format_unsigned_arg(&ap, length), false, base,
                           width, precision, flags);
            break;
        case 'f':
            format_float(&out, va_arg(ap, double), width, precision, flags);
            break;
        case '%':
            format_char(&out, '%');
            break;
        default:
            format_char(&out, '%');
            format_char(&out, spec);
            break;
        }
    }
    va_end(ap);
    if (buf != NULL && size != 0) {
        size_t end = out.count < size ? out.count : size - 1;
        buf[end] = '\0';
    }
    return out.count > INT_MAX ? INT_MAX : (int)out.count;
}

__printf(3, 4) int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    int len;
    va_list args;

    va_start(args, fmt);
    len = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return len;
}

__printf(2, 0) int vsprintf(char *buf, const char *fmt, va_list args)
{
    return vsnprintf(buf, SIZE_MAX, fmt, args);
}

__printf(2, 3) int sprintf(char *buf, const char *fmt, ...)
{
    int len;
    va_list args;

    va_start(args, fmt);
    len = vsprintf(buf, fmt, args);
    va_end(args);
    return len;
}
